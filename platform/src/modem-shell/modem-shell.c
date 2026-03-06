#include <modem-board.h>

#include <errno.h>
#include <string.h>

#include <zephyr/shell/shell.h>

static int cmd_modem_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct modem_board_status st;
	int ret = modem_board_get_status(&st);
	if (ret != 0) {
		shell_error(sh, "status read failed: %d", ret);
		return ret;
	}

	shell_print(sh,
		"MODEM_3V8_EN=%d, MODEM_PWR_ON_N=%d, MODEM_RST_N=%d",
		st.rail_en, st.pwr_on_n, st.rst_n);
	return 0;
}

static int cmd_modem_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = modem_board_reset_pulse();
	if (ret != 0) {
		shell_error(sh, "reset failed: %d", ret);
		return ret;
	}

	shell_print(sh, "OK");
	return 0;
}

static int cmd_modem_power(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: modem power <on|off|cycle>");
		return -EINVAL;
	}

	const char *op = argv[1];
	int ret = 0;

	if (strcmp(op, "on") == 0) {
		ret = modem_board_power_on();
	} else if (strcmp(op, "off") == 0) {
		ret = modem_board_power_off();
	} else if (strcmp(op, "cycle") == 0) {
		ret = modem_board_power_cycle();
	} else {
		shell_error(sh, "unknown power op: %s", op);
		return -EINVAL;
	}

	if (ret != 0) {
		shell_error(sh, "power %s failed: %d", op, ret);
		return ret;
	}

	shell_print(sh, "OK");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_modem,
	SHELL_CMD(status, NULL, "Print modem GPIO status", cmd_modem_status),
	SHELL_CMD(reset, NULL, "Pulse modem reset (MODEM_nRST)", cmd_modem_reset),
	SHELL_CMD_ARG(power, NULL, "Modem power control: power <on|off|cycle>", cmd_modem_power, 2, 0),
	SHELL_SUBCMD_SET_END /* Array terminator */
);

SHELL_CMD_REGISTER(modem, &sub_modem, "Modem control", NULL);
