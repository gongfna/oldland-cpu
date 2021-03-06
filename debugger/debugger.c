#define _GNU_SOURCE

#include <argp.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <readline/readline.h>
#include <readline/history.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "breakpoint.h"
#include "debugger.h"
#include "protocol.h"
#include "loadsyms.h"

#define NUM_HISTORY_LINES	1000
#define PSR_BASE		32

#ifndef INSTALL_PATH
#define INSTALL_PATH "/usr/local"
#endif /* !INSTALL_PATH */

const char *argp_program_version = "0.1";
const char *argp_program_bug_address = "jamie@jamieiles.com";
static char doc[] = "Oldland CPU debugger.";

struct target {
	int fd;
	bool interrupted;

	bool addr_written;
	uint32_t cached_addr;

	bool wdata_written;
	uint32_t cached_wdata;

	bool mem_written;

	bool breakpoint_hit;

	uint32_t psr;

	/* Populated when stopped. */
	uint32_t pc;

	struct regcache *regcache;
};

static struct target *target;
static bool interactive;

static int target_exchange(const struct target *t,
			   const struct dbg_request *req,
			   struct dbg_response *resp)
{
	ssize_t rc;
	struct iovec reqv = {
		.iov_base = (void *)req,
		.iov_len = sizeof(*req)
	};
	struct iovec respv = {
		.iov_base = resp,
		.iov_len = sizeof(*resp)
	};

	rc = writev(t->fd, &reqv, 1);
	if (rc < 0)
		return -EIO;
	if (rc != (ssize_t)sizeof(*req))
		return -EIO;

	rc = readv(t->fd, &respv, 1);
	if (rc < 0)
		return rc;
	if (rc != (ssize_t)sizeof(*resp))
		return -EIO;

	return 0;
}

static int dbg_write(struct target *t, enum dbg_reg addr, uint32_t value)
{
	struct dbg_request req = {
		.addr = addr,
		.value = value,
		.read_not_write = 0,
	};
	struct dbg_response resp;
	int rc;

	if (addr == REG_ADDRESS) {
		if (value == t->cached_addr && t->addr_written)
			return 0;
		t->cached_addr = value;
		t->addr_written = true;
	}

	if (addr == REG_WDATA) {
		if (value == t->cached_wdata && t->wdata_written)
			return 0;
		t->cached_wdata = value;
		t->wdata_written = true;
	}

	rc = target_exchange(t, &req, &resp);
	if (!rc)
		rc = resp.status;

	return rc;
}

static int dbg_read(struct target *t, enum dbg_reg addr, uint32_t *value)
{
	struct dbg_request req = {
		.addr = addr,
		.read_not_write = 1,
	};
	struct dbg_response resp;
	int rc;

	rc = target_exchange(t, &req, &resp);
	if (!rc)
		rc = resp.status;
	*value = resp.data;

	return rc;
}

static int dbg_term(struct target *t)
{
	return dbg_write(t, REG_CMD, CMD_SIM_TERM);
}

static int dbg_start_trace(struct target *t)
{
	return dbg_write(t, REG_CMD, CMD_START_TRACE);
}

int dbg_stop(struct target *t)
{
	int rc = dbg_write(t, REG_CMD, CMD_STOP);

	if (!rc)
		rc = dbg_read(t, REG_RDATA, &t->pc);

	return rc;
}

static int dbg_cache_sync(struct target *t)
{
	int rc;

	if (!t->mem_written)
		return 0;

	rc = dbg_write(t, REG_CMD, CMD_CACHE_SYNC);
	if (!rc)
		t->mem_written = 0;

	return rc;
}

int dbg_run(struct target *t)
{
	int rc = regcache_sync(t->regcache);

	if (!rc)
		rc = dbg_cache_sync(t);
	if (!rc)
		rc = dbg_write(t, REG_CMD, CMD_RUN);

	return rc;
}

int dbg_step(struct target *t)
{
	int rc = regcache_sync(t->regcache);

	if (!rc)
		rc = dbg_cache_sync(t);
	if (!rc)
		rc = dbg_write(t, REG_CMD, CMD_STEP);
	if (!rc)
		rc = dbg_read(t, REG_RDATA, &t->pc);

	return rc;
}

static int dbg_reset(struct target *t)
{
	int rc = regcache_sync(t->regcache);

	if (!rc)
		dbg_stop(t);
	if (!rc)
		dbg_cache_sync(t);
	if (rc)
		return rc;

	return dbg_write(t, REG_CMD, CMD_RESET);
}

/*
 * Forcibly reload the cached copy of the PC.  For run() and step() the debug
 * controller returns the updated PC, but when execution has hit a breakpoint
 * we just get that by polling the execution status so need to manually update
 * the PC.
 */
static int dbg_reload_pc(struct target *t)
{
	int rc = dbg_write(t, REG_ADDRESS, PC);

	if (rc)
		return rc;
	rc = dbg_write(t, REG_CMD, CMD_READ_REG);
	if (rc)
		return rc;

	return dbg_read(t, REG_RDATA, &t->pc);
}

int dbg_read_reg(struct target *t, unsigned reg, uint32_t *val)
{
	int rc;

	if (reg == PC) {
		*val = t->pc;
		return 0;
	}

	rc = dbg_write(t, REG_ADDRESS, reg);
	if (rc)
		return rc;
	rc = dbg_write(t, REG_CMD, CMD_READ_REG);
	if (rc)
		return rc;

	return dbg_read(t, REG_RDATA, val);
}

int dbg_read_cpuid(struct target *t, unsigned reg, uint32_t *val)
{
	int rc;

	rc = dbg_write(t, REG_ADDRESS, reg);
	if (rc)
		return rc;
	rc = dbg_write(t, REG_CMD, CMD_CPUID);
	if (rc)
		return rc;

	return dbg_read(t, REG_RDATA, val);
}

int dbg_get_exec_status(struct target *t, uint32_t *status)
{
	int rc;

	rc = dbg_write(t, REG_CMD, CMD_GET_EXEC_STATUS);
	if (rc)
		return rc;

	return dbg_read(t, REG_RDATA, status);
}

static void assert_target(lua_State *L)
{
	if (!target) {
		lua_pushstring(L, "not connected");
		lua_error(L);
	}
}

#define MEM_READ_FN(width)							\
int dbg_read##width(struct target *t, unsigned addr, uint32_t *val)		\
{										\
	int rc;									\
										\
	rc = dbg_cache_sync(target);						\
	if (rc)									\
		return rc;							\
										\
	rc = dbg_write(t, REG_ADDRESS, addr);					\
	if (rc)									\
		return rc;							\
	rc = dbg_write(t, REG_CMD, CMD_RMEM##width);				\
	if (rc)									\
		return rc;							\
										\
	return dbg_read(t, REG_RDATA, val);					\
}										\
										\
static int lua_read##width(lua_State *L)					\
{										\
	uint32_t v;								\
	lua_Integer addr;							\
										\
	assert_target(L);							\
										\
	if (lua_gettop(L) != 1) {						\
		lua_pushstring(L, "no address provided");			\
		lua_error(L);							\
	}									\
										\
	addr = lua_tointeger(L, 1);						\
	if (dbg_read##width(target, addr, &v))					\
		warnx("failed to read " #width "-bit address %u",		\
		      (unsigned)addr);						\
	v &= (uint32_t)((1LU << width) - 1LU);					\
	lua_pop(L, 1);								\
	lua_pushinteger(L, v);							\
										\
	return 1;								\
}

#define MEM_WRITE_FN(width)							\
int dbg_write##width(struct target *t, unsigned addr, uint32_t val)		\
{										\
	int rc;									\
										\
	rc = dbg_write(t, REG_ADDRESS, addr);					\
	if (rc)									\
		return rc;							\
	rc = dbg_write(t, REG_WDATA, val);					\
	if (rc)									\
		return rc;							\
										\
	rc = dbg_write(t, REG_CMD, CMD_WMEM##width);				\
	if (rc)									\
		return rc;							\
										\
	t->mem_written = 1;							\
										\
	return rc;								\
}										\
										\
static int lua_write##width(lua_State *L)					\
{										\
	lua_Integer addr, val;							\
										\
	assert_target(L);							\
										\
	if (lua_gettop(L) != 2) {						\
		lua_pushstring(L, "no address/value provided");			\
		lua_error(L);							\
	}									\
										\
	addr = lua_tointeger(L, 1);						\
	val = lua_tointeger(L, 2);						\
	if (dbg_write##width(target, addr, val))				\
		warnx("failed to write " #width "-bit address  %u",		\
		      (unsigned)addr);						\
	lua_pop(L, 2);								\
										\
	return 0;								\
}

MEM_READ_FN(32);
MEM_READ_FN(16);
MEM_READ_FN(8);
MEM_WRITE_FN(32);
MEM_WRITE_FN(16);
MEM_WRITE_FN(8);

int dbg_write_reg(struct target *t, unsigned reg, uint32_t val)
{
	int rc;

	rc = dbg_write(t, REG_ADDRESS, reg);
	if (rc)
		return rc;
	rc = dbg_write(t, REG_WDATA, val);
	if (rc)
		return rc;

	rc = dbg_write(t, REG_CMD, CMD_WRITE_REG);
	if (!rc && reg == PC)
		t->pc = val;

	return rc;
}

int open_server(const char *hostname, const char *port)
{
	struct addrinfo *result, *rp, hints = {
		.ai_family	= AF_INET,
		.ai_socktype	= SOCK_STREAM,
	};
	int s, fd;

	s = getaddrinfo(hostname, port, &hints, &result);
	if (s)
		return -errno;

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, rp->ai_addr, rp->ai_addrlen) >= 0)
			break;
		close(fd);
	}

	freeaddrinfo(result);

	if (!rp)
		return -EADDRNOTAVAIL;

	return fd;
}

static struct target *target_alloc(const char *hostname,
				   const char *port)
{
	struct target *t = calloc(1, sizeof(*t));

	if (!t)
		err(1, "failed to allocate target");

	t->fd = open_server(hostname, port);
	if (t->fd < 0) {
		warn("failed to connect to server");
		free(t);
		return NULL;
	}

	t->regcache = regcache_new(t);
	if (!t->regcache) {
		close(t->fd);
		free(t);
		t = NULL;
	}

	return t;
}

static void disable_mmu(struct target *t)
{
	uint32_t psr;

	if (dbg_read_reg(t, CR_BASE + 1, &psr))
		err(1, "failed to read psr");
	target->psr = psr;
	psr &= ~(1 << 7);

	if (dbg_write_reg(t, CR_BASE + 1, psr))
		err(1, "failed to write psr");
}

static void restore_mmu(struct target *t)
{
	if (dbg_write_reg(t, CR_BASE + 1, t->psr))
		err(1, "failed to write psr");
}

static void wait_until_stopped(struct target *t)
{
	target->interrupted = false;
	uint32_t exec_status = 0;

	do {
		if (dbg_get_exec_status(target, &exec_status))
			err(1, "failed to get execution status.");
	} while (!target->interrupted && (exec_status & EXEC_STATUS_RUNNING));

	if (dbg_reload_pc(t))
		err(1, "failed to read PC");

	target->breakpoint_hit = exec_status & EXEC_STATUS_STOPPED_ON_BKPT;
}

static void do_exec(struct target *target,
		    int (*fn)(struct target *))
{
	struct breakpoint *bkp;

	bkp = breakpoint_at_addr(target->pc);
	if (bkp)
		breakpoint_exec_orig(bkp);

	restore_mmu(target);
	if (fn(target))
		warnx("failed to step target");

	wait_until_stopped(target);
	disable_mmu(target);

	bkp = breakpoint_at_addr(target->pc);
	if (bkp)
		printf("breakpoint %d hit at %08x\n", bkp->id, bkp->addr);
}

static int lua_step(lua_State *L)
{
	assert_target(L);

	do_exec(target, dbg_step);

	return 0;
}

static int lua_term(lua_State *L)
{
	assert_target(L);

	(void)dbg_term(target);

	return 0;
}

static int lua_start_trace(lua_State *L)
{
	assert_target(L);

	(void)dbg_start_trace(target);

	return 0;
}

static int lua_stop(lua_State *L)
{
	assert_target(L);

	if (dbg_stop(target))
		warnx("failed to step target");
	disable_mmu(target);

	return 0;
}

static int lua_run(lua_State *L)
{
	assert_target(L);

	do_exec(target, dbg_run);

	return 0;
}

static int lua_reset(lua_State *L)
{
	assert_target(L);

	if (dbg_reset(target))
		warnx("failed to reset target");
	disable_mmu(target);

	return 0;
}

static int lua_read_reg(lua_State *L)
{
	uint32_t v;
	lua_Integer regnum;

	assert_target(L);

	if (lua_gettop(L) != 1) {
		lua_pushstring(L, "no register identifier");
		lua_error(L);
	}

	regnum = lua_tointeger(L, 1);
	if (regcache_read(target->regcache, regnum, &v))
		warnx("failed to read register %u", (unsigned)regnum);
	lua_pop(L, 1);
	lua_pushinteger(L, v);

	return 1;
}

static int lua_read_cpuid(lua_State *L)
{
	uint32_t v;
	lua_Integer regnum;

	assert_target(L);

	if (lua_gettop(L) != 1) {
		lua_pushstring(L, "no register identifier");
		lua_error(L);
	}

	regnum = lua_tointeger(L, 1);
	if (dbg_read_cpuid(target, regnum, &v))
		warnx("failed to read cpuid register %u", (unsigned)regnum);
	lua_pop(L, 1);
	lua_pushinteger(L, v);

	return 1;
}

static int lua_set_bkp(lua_State *L)
{
	uint32_t addr;
	struct breakpoint *bkp;

	assert_target(L);

	if (lua_gettop(L) != 1) {
		lua_pushstring(L, "no breakpoint address");
		lua_error(L);
	}

	addr = lua_tointeger(L, 1);
	bkp = breakpoint_register(target, addr);
	if (!bkp) {
		lua_pushstring(L, "failed to set breakpoint");
		lua_error(L);
	}

	lua_pop(L, 1);
	lua_pushinteger(L, bkp->id);

	return 1;
}

static int lua_del_bkp(lua_State *L)
{
	int id;
	struct breakpoint *bkp;

	assert_target(L);

	if (lua_gettop(L) != 1) {
		lua_pushstring(L, "no breakpoint id");
		lua_error(L);
	}

	id = lua_tointeger(L, 1);
	bkp = breakpoint_get(id);
	if (!bkp) {
		lua_pushstring(L, "failed to delete breakpoint");
		lua_error(L);
	}

	breakpoint_remove(bkp);
	lua_pop(L, 1);

	return 0;
}

static int lua_write_reg(lua_State *L)
{
	lua_Integer regnum, val;

	assert_target(L);

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "no register identifier/value");
		lua_error(L);
	}

	regnum = lua_tointeger(L, 1);
	val = lua_tointeger(L, 2);
	if (regcache_write(target->regcache, regnum, val))
		warnx("failed to write register %u", (unsigned)regnum);
	lua_pop(L, 2);

	return 0;
}

static void set_symbols(lua_State *L, const char *path)
{
	struct symtab *symtab = load_symbols(path);
	unsigned m;

	if (!symtab) {
		warnx("failed to load symbols for %s", path);
		return;
	}

	lua_newtable(L);
	for (m = 0; m < symtab->nr_syms; ++m) {
		lua_pushstring(L, symtab->syms[m].name);
		lua_pushinteger(L, symtab->syms[m].value);
		lua_settable(L, -3);
	}
	lua_setglobal(L, "syms");

	free_symbols(symtab);
}

static int lua_loadsyms(lua_State *L)
{
	const char *path;

	assert_target(L);

	if (lua_gettop(L) != 1) {
		lua_pushstring(L, "no elf file provided.");
		lua_error(L);
	}

	path = lua_tostring(L, 1);
	set_symbols(L, path);

	return 0;
}

static void push_testpoint(lua_State *L, const struct testpoint *tp)
{
	lua_pushinteger(L, tp->addr);
	lua_newtable(L);

	lua_pushstring(L, "type");
	lua_pushinteger(L, tp->type);
	lua_settable(L, -3);

	lua_pushstring(L, "tag");
	lua_pushinteger(L, tp->tag);
	lua_settable(L, -3);

	/* Set the testpoint in the table. */
	lua_settable(L, -3);
}

static int lua_loadelf(lua_State *L)
{
	const char *path;
	struct testpoint *testpoints;
	size_t nr_testpoints;
	size_t n;

	assert_target(L);

	if (lua_gettop(L) != 1) {
		lua_pushstring(L, "no elf file provided.");
		lua_error(L);
	}

	path = lua_tostring(L, 1);
	if (load_elf(target, path, &testpoints, &nr_testpoints))
		warnx("failed to load device with %s", path);
	lua_pop(L, 1);

	set_symbols(L, path);

	lua_newtable(L);
	for (n = 0; n < nr_testpoints; ++n)
		push_testpoint(L, &testpoints[n]);

	lua_setglobal(L, "testpoints");

	return 0;
}

static int lua_connect(lua_State *L)
{
	const char *host, *port;

	if (lua_gettop(L) != 2) {
		lua_pushstring(L, "host and port required");
		lua_error(L);
	}

	host = lua_tostring(L, 1);
	port = lua_tostring(L, 2);

	target = target_alloc(host, port) ;
	if (!target) {
		lua_pushstring(L, "failed to connect to host");
		lua_error(L);
	}

	if (dbg_stop(target)) {
		lua_pushstring(L, "failed to stop target");
		lua_error(L);
	}

	if (dbg_reset(target)) {
		lua_pushstring(L, "failed to reset target");
		lua_error(L);
	}

	if (interactive) {
		lua_getglobal(L, "report_cpu");
		if (lua_pcall(L, 0, 0, 0))
			errx(1, "failed to get CPU data (%s)", lua_tostring(L, -1));
	}

	return 0;
}

static const struct luaL_Reg dbg_funcs[] = {
	{ "step", lua_step },
	{ "run", lua_run },
	{ "stop", lua_stop },
	{ "read_reg", lua_read_reg },
	{ "write_reg", lua_write_reg },
	{ "read32", lua_read32 },
	{ "write32", lua_write32 },
	{ "read16", lua_read16 },
	{ "write16", lua_write16 },
	{ "read8", lua_read8 },
	{ "write8", lua_write8 },
	{ "loadelf", lua_loadelf },
	{ "loadsyms", lua_loadsyms },
	{ "connect", lua_connect },
	{ "term", lua_term },
	{ "start_trace", lua_start_trace },
	{ "reset", lua_reset },
	{ "read_cpuid", lua_read_cpuid },
	{ "set_bkp", lua_set_bkp },
	{ "del_bkp", lua_del_bkp },
	{}
};

static void load_support(lua_State *L)
{
	char *path = NULL;

	if (asprintf(&path, "%s/libexec/oldland-debug-ui.lua",
		     INSTALL_PATH) < 0)
		err(1, "failed to allocate support path");

	if (luaL_dofile(L, path))
		errx(1, "failed to load support (%s)", lua_tostring(L, -1));

	free(path);
}

static void sigint_handler(int s)
{
	if (target)
		target->interrupted = true;

	if (target)
		dbg_stop(target);
}

static void run_interactive(lua_State *L)
{
	char *history_path = NULL;

	if (asprintf(&history_path, "%s/.oldland-debug_history", getpwuid(getuid())->pw_dir) < 0)
		abort();

	using_history();
	read_history(history_path);

	signal(SIGINT, sigint_handler);

	for (;;) {
		char *line = readline("oldland> ");

		if (!line)
			break;

		if (luaL_dostring(L, line))
			warnx("error: %s", lua_tostring(L, -1));

		add_history(line);
	}

	append_history(NUM_HISTORY_LINES, history_path);
	history_truncate_file(history_path, NUM_HISTORY_LINES);

	free(history_path);
}

static void run_command_script(lua_State *L, const char *path)
{
	fflush(stdout);
	fflush(stderr);

	if (luaL_dofile(L, path)) {
		warnx("failed to run command script %s", path);
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		exit(EXIT_FAILURE);
	}

	fflush(stdout);
	fflush(stderr);

	exit(lua_gettop(L) == 1 ? lua_tointeger(L, 1) : 0);
}

static struct argp_option options[] = {
	{ "command", 'x', "FILE", 0 },
	{ "startup", 's', "FILE", 0 },
	{}
};

struct arguments {
	const char *command_script;
	const char *startup_script;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct arguments *args = state->input;

	switch (key) {
	case 'x':
		args->command_script = arg;
		break;
	case 's':
		args->startup_script = arg;
		break;
	case ARGP_KEY_ARG:
	case ARGP_KEY_END:
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}

	return 0;
}

static struct argp argp = { options, parse_opt, NULL, doc };

int main(int argc, char *argv[])
{
	struct arguments args = {};

	argp_parse(&argp, argc, argv, 0, 0, &args);

	lua_State *L = luaL_newstate();

	assert(L);

	luaL_openlibs(L);
	luaL_newlib(L, dbg_funcs);
	lua_setglobal(L, "target");
	load_support(L);

	if (args.command_script) {
		run_command_script(L, args.command_script);
	} else {
		interactive = true;
		if (args.startup_script) {
			if (luaL_dofile(L, args.startup_script)) {
				fprintf(stderr, "%s\n", lua_tostring(L, -1));
				exit(EXIT_FAILURE);
			}
		}

		run_interactive(L);
	}

	if (target)
		dbg_run(target);

	fflush(stdout);

	return 0;
}
