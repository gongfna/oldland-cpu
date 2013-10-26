require "common"

MAX_CYCLE_COUNT = 128

connect_and_load("irq")

expect_testpoints = {
	{ TP_USER, 2 },
	{ TP_USER, 0 },
	{ TP_USER, 2 },
	{ TP_USER, 1 },
	{ TP_USER, 2 },
	{ TP_USER, 0 },
	{ TP_USER, 2 },
	{ TP_USER, 1 },
	{ TP_SUCCESS, 0 },
}

return run_testpoints(expect_testpoints)