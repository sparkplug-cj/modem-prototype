#include "modem-shell-core.h"

#include <errno.h>
#include <string.h>

#define MODEM_AT_SYNC_RETRIES 3
#define MODEM_AT_BOOT_DELAY_MS 5000
#define MODEM_AT_SYNC_COMMAND "AT"
#define MODEM_AT_DISABLE_SLEEP_COMMAND "AT+KSLEEP=2"

static int modem_shell_disable_sleep_after_power_on(const struct modem_shell_ops *ops)
{
	char response[256];
	int ret = -ETIMEDOUT;
	int (*send_fn)(const char *command, char *response, size_t responseSize) =
		ops->modem_at_send_power_on != NULL ? ops->modem_at_send_power_on :
		(ops->modem_at_send_runtime != NULL ? ops->modem_at_send_runtime : ops->modem_at_send);

	ops->print(ops->ctx, "Waiting for modem boot...");
	ops->sleep_ms(MODEM_AT_BOOT_DELAY_MS);

	for (int attempt = 0; attempt < MODEM_AT_SYNC_RETRIES; ++attempt) {
		ret = send_fn(MODEM_AT_SYNC_COMMAND, response, sizeof(response));
		if (ret == 0) {
			break;
		}
	}

	if (ret != 0) {
		ops->error(ops->ctx, "AT sync failed after power-on: %d", ret);
		return ret;
	}

	ops->print(ops->ctx, "Disabling sleep...");
	ret = send_fn(MODEM_AT_DISABLE_SLEEP_COMMAND, response, sizeof(response));
	if (ret != 0) {
		ops->error(ops->ctx, "failed to disable modem sleep: %d", ret);
		return ret;
	}

	return 0;
}

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
			"MODEM_3V8_EN=%d, MODEM_PWR_ON=%d, MODEM_RST=%d, VGPIO_mV=ERR(%d), MODEM_STATE=OFF",
			st.rail_en, st.pwr_on, st.rst, st.vgpio_mv);
		return 0;
	}

	ops->print(ops->ctx,
		"MODEM_3V8_EN=%d, MODEM_PWR_ON=%d, MODEM_RST=%d, VGPIO_mV=%d, MODEM_STATE=%s",
		st.rail_en, st.pwr_on, st.rst, st.vgpio_mv, st.modem_state_on ? "ON" : "OFF");
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
	bool shouldDisableSleep = false;

	if (strcmp(op, "on") == 0) {
		ops->print(ops->ctx, "Powering modem...");
		ret = ops->modem_board_power_on();
		shouldDisableSleep = true;
	} else if (strcmp(op, "off") == 0) {
		ret = ops->modem_board_power_off();
	} else if (strcmp(op, "cycle") == 0) {
		ops->print(ops->ctx, "Powering modem...");
		ret = ops->modem_board_power_cycle();
		shouldDisableSleep = true;
	} else {
		ops->error(ops->ctx, "unknown power op: %s", op);
		return -EINVAL;
	}

	if (ret != 0) {
		ops->error(ops->ctx, "power %s failed: %d", op, ret);
		return ret;
	}

	if (shouldDisableSleep) {
		ret = modem_shell_disable_sleep_after_power_on(ops);
		if (ret != 0) {
			return ret;
		}
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

	int (*send_fn)(const char *command, char *response, size_t responseSize) =
		ops->modem_at_send_runtime != NULL ? ops->modem_at_send_runtime : ops->modem_at_send;

	ret = send_fn(argv[1], response, sizeof(response));
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
