#include "modem-shell-core.h"

#include <errno.h>
#include <string.h>

int modem_shell_cmd_status_core(const struct modem_shell_ops *ops)
{
	struct modem_board_status st;
	int ret = ops->modem_board_get_status(&st);
	if (ret != 0) {
		ops->error(ops->ctx, "status read failed: %d", ret);
		return ret;
	}

	if (st.vgpio_mv < 0) {
		ops->print(ops->ctx,
			"MODEM_3V8_EN=%d, MODEM_PWR_ON_N=%d, MODEM_RST_N=%d, VGPIO_mV=ERR(%d), MODEM_STATE=OFF",
			st.rail_en, st.pwr_on_n, st.rst_n, st.vgpio_mv);
		return 0;
	}

	ops->print(ops->ctx,
		"MODEM_3V8_EN=%d, MODEM_PWR_ON_N=%d, MODEM_RST_N=%d, VGPIO_mV=%d, MODEM_STATE=%s",
		st.rail_en, st.pwr_on_n, st.rst_n, st.vgpio_mv, st.modem_state_on ? "ON" : "OFF");
	return 0;
}

int modem_shell_cmd_reset_core(const struct modem_shell_ops *ops)
{
	int ret = ops->modem_board_reset_pulse();
	if (ret != 0) {
		ops->error(ops->ctx, "reset failed: %d", ret);
		return ret;
	}

	ops->print(ops->ctx, "OK");
	return 0;
}

int modem_shell_cmd_power_core(const struct modem_shell_ops *ops, size_t argc, char **argv)
{
	if (argc < 2U) {
		ops->error(ops->ctx, "usage: modem power <on|off|cycle>");
		return -EINVAL;
	}

	const char *op = argv[1];
	int ret = 0;

	if (strcmp(op, "on") == 0) {
		ret = ops->modem_board_power_on();
	} else if (strcmp(op, "off") == 0) {
		ret = ops->modem_board_power_off();
	} else if (strcmp(op, "cycle") == 0) {
		ret = ops->modem_board_power_cycle();
	} else {
		ops->error(ops->ctx, "unknown power op: %s", op);
		return -EINVAL;
	}

	if (ret != 0) {
		ops->error(ops->ctx, "power %s failed: %d", op, ret);
		return ret;
	}

	ops->print(ops->ctx, "OK");
	return 0;
}

int modem_shell_cmd_at_core(const struct modem_shell_ops *ops, size_t argc, char **argv)
{
	struct modem_board_status st;
	struct modem_at_diagnostics diagnostics;
	char response[256];
	int ret;

	if (argc < 2U) {
		ops->error(ops->ctx, "usage: at <command>");
		return -EINVAL;
	}

	ret = ops->modem_board_get_status(&st);
	if (ret != 0) {
		ops->error(ops->ctx, "status read failed: %d", ret);
		return ret;
	}

	if (st.rail_en != 1) {
		ops->error(ops->ctx, "modem is not powered");
		return -EHOSTDOWN;
	}

	ret = ops->modem_at_send(argv[1], response, sizeof(response));
	modem_at_get_last_diagnostics(&diagnostics);
	if (ret != 0) {
		if (ret == -ETIMEDOUT) {
			ops->error(ops->ctx,
				"AT command timed out waiting for modem response (exit=%s, bytes=%u)",
				modem_at_exit_reason_str(diagnostics.exitReason),
				(unsigned int)diagnostics.bytesReceived);
		} else {
			ops->error(ops->ctx,
				"AT command failed: %d (exit=%s, bytes=%u)",
				ret,
				modem_at_exit_reason_str(diagnostics.exitReason),
				(unsigned int)diagnostics.bytesReceived);
		}
		return ret;
	}

	if (response[0] == '\0') {
		ops->print(ops->ctx,
			"[empty modem response]\n[modem-at] exit=%s bytes=%u",
			modem_at_exit_reason_str(diagnostics.exitReason),
			(unsigned int)diagnostics.bytesReceived);
		return 0;
	}

	if (strcmp(response, argv[1]) == 0) {
		ops->print(ops->ctx,
			"[echo only]\n%s\n[modem-at] exit=%s bytes=%u",
			response,
			modem_at_exit_reason_str(diagnostics.exitReason),
			(unsigned int)diagnostics.bytesReceived);
		return 0;
	}

	ops->print(ops->ctx,
		"[raw modem response]\n%s\n[modem-at] exit=%s bytes=%u",
		response,
		modem_at_exit_reason_str(diagnostics.exitReason),
		(unsigned int)diagnostics.bytesReceived);
	return 0;
}
