#include <modem-board.h>

#include "modem-shell-core.h"

#include <zephyr/shell/shell.h>

static const struct modem_shell_ops shellOps = {
	.modem_board_power_on = modem_board_power_on,
	.modem_board_power_off = modem_board_power_off,
	.modem_board_power_cycle = modem_board_power_cycle,
	.modem_board_reset_pulse = modem_board_reset_pulse,
	.modem_board_get_status = modem_board_get_status,
	.print = (void (*)(void *, const char *, ...))shell_print,
	.error = (void (*)(void *, const char *, ...))shell_error,
};

static int cmd_modem_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct modem_shell_ops ops = shellOps;
	ops.ctx = (void *)sh;
	return modem_shell_cmd_status_core(&ops);
}

static int cmd_modem_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct modem_shell_ops ops = shellOps;
	ops.ctx = (void *)sh;
	return modem_shell_cmd_reset_core(&ops);
}

static int cmd_modem_power(const struct shell *sh, size_t argc, char **argv)
{
	struct modem_shell_ops ops = shellOps;
	ops.ctx = (void *)sh;
	return modem_shell_cmd_power_core(&ops, argc, argv);
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_modem,
	SHELL_CMD(status, NULL, "Print modem GPIO status", cmd_modem_status),
	SHELL_CMD(reset, NULL, "Pulse modem reset (MODEM_nRST)", cmd_modem_reset),
	SHELL_CMD_ARG(power, NULL, "Modem power control: power <on|off|cycle>", cmd_modem_power, 2, 0),
	SHELL_SUBCMD_SET_END /* Array terminator */
);

SHELL_CMD_REGISTER(modem, &sub_modem, "Modem control", NULL);
