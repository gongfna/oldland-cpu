#define DEBUG

#define _GNU_SOURCE
#include <assert.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lua5.2/lua.h>
#include <lua5.2/lauxlib.h>
#include <lua5.2/lualib.h>

#include "cpu.h"
#include "internal.h"
#include "io.h"
#include "trace.h"

static lua_State *lua_interp;

static const char *test_get_bin(const char *test_file)
{
	char *ret = NULL;

	lua_getglobal(lua_interp, "BINARY");
	if (!lua_isnil(lua_interp, -1)) {
		char *tmp = strdup(test_file);

		assert(tmp != NULL);
		if (asprintf(&ret, "%s/%s", dirname(tmp),
			     lua_tolstring(lua_interp, -1, NULL)) < 0)
			die("failed to allocate binary path\n");
		free(tmp);
	}

	lua_pop(lua_interp, 1);

	return ret;
}

static int lua_sim_err(lua_State *L)
{
	const char *msg = lua_tolstring(L, -1, NULL);

	die("%s\n", msg);

	return 0;
}

static const struct luaL_Reg sim_funcs[] = {
	{ "err", lua_sim_err },
	{}
};

static lua_State *init_test_script(const char *test_file)
{
	lua_State *L = luaL_newstate();

	assert(L);
	luaL_openlibs(L);
	luaL_newlib(L, sim_funcs);
	lua_setglobal(L, "sim");

	if (luaL_dofile(L, test_file))
		die("failed to load test %s (%s)\n", test_file,
		    lua_tostring(L, -1));

	return L;
}

static void validate_result(struct cpu *c)
{
	lua_getglobal(lua_interp, "validate_result");
	if (!lua_isnil(lua_interp, -1))
		lua_call(lua_interp, 0, 0);
	else
		lua_pop(lua_interp, 1);
}

void cpu_mem_write_hook(struct cpu *c, physaddr_t addr, unsigned int nr_bits,
			uint32_t val)
{
	lua_getglobal(lua_interp, "data_write_hook");
	if (lua_isnil(lua_interp, -1)) {
		lua_pop(lua_interp, 1);
		return;
	}

	lua_pushinteger(lua_interp, addr);
	lua_pushinteger(lua_interp, nr_bits);
	lua_pushinteger(lua_interp, val);
	lua_call(lua_interp, 3, 0);
}

int main(int argc, char *argv[])
{
	struct cpu *c;
	int err;

	if (argc < 2)
		die("usage: %s TEST_FILE\n", argv[0]);

	lua_interp = init_test_script(argv[1]);
	assert(lua_interp);

	c = new_cpu(argv[1], test_get_bin(argv[1]));
	printf("Oldland CPU simulator\n");

	do {
		err = cpu_cycle(c);
	} while (err == 0);

	printf("[%s]\n", err == SIM_SUCCESS ? "SUCCESS" : "FAIL");
	if (err == SIM_SUCCESS)
		validate_result(c);

	lua_close(lua_interp);

	return err == SIM_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
}
