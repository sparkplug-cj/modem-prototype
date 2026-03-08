#include "modem-shell-core.h"

#include "modem-at.h"
#include "modem-board.h"

#include <stdarg.h>
#include <stdio.h>
#include <zephyr/shell/shell.h>

static void shell_print_adapter(void *ctx, const char *fmt, ...)
{
	const struct shell *sh = ctx;
	char buffer[128];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	shell_fprintf_normal(sh, "%s\n", buffer);
}

static void shell_error_adapter(void *ctx, const char *fmt, ...)
{
	const struct shell *sh = ctx;
	char buffer[128];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	shell_fprintf_error(sh, "%s\n", buffer);
}

static const struct modem_shell_ops shellOps = {
	.modem_board_power_on = modem_board_power_on,
	.modem_board_power_off = modem_board_power_off,
	.modem_board_power_cycle = modem_board_power_cycle,
	.modem_board_reset_pulse = modem_board_reset_pulse,
	.modem_board_get_status = modem_board_get_status,
	.modem_at_send = modem_at_send,
	.print = shell_print_adapter,
	.error = shell_error_adapter,
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

static int cmd_modem_at(const struct shell *sh, size_t argc, char **argv)
{
	struct modem_shell_ops ops = shellOps;
	ops.ctx = (void *)sh;
	return modem_shell_cmd_at_core(&ops, argc, argv);
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_modem,
	SHELL_CMD(status, NULL, "Print modem GPIO status", cmd_modem_status),
	SHELL_CMD(reset, NULL, "Pulse modem reset (MODEM_nRST)", cmd_modem_reset),
	SHELL_CMD_ARG(power, NULL, "Modem power control: power <on|off|cycle>", cmd_modem_power, 2, 0),
	SHELL_CMD_ARG(at, NULL, "Send AT command: at <command>", cmd_modem_at, 2, 0),
	SHELL_SUBCMD_SET_END /* Array terminator */
);

SHELL_CMD_REGISTER(modem, &sub_modem, "Modem control", NULL);
