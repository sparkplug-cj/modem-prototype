#include "modem-shell-core.h"

#include "modem-at.h"
#include "modem-board.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#define MODEM_PASSTHROUGH_STACK_SIZE 1024
#define MODEM_PASSTHROUGH_THREAD_PRIORITY 7
#define MODEM_PASSTHROUGH_ESCAPE_PREFIX 0x18u
#define MODEM_PASSTHROUGH_ESCAPE_SUFFIX 0x11u
#define MODEM_PASSTHROUGH_RX_CHUNK_SIZE 64
#define MODEM_PASSTHROUGH_RX_GATHER_WINDOW_MS 10
#define MODEM_PASSTHROUGH_RX_TRACE_BUFFER_SIZE (MODEM_PASSTHROUGH_RX_CHUNK_SIZE * 3U + 1U)

static void shell_print_adapter(void *ctx, const char *fmt, ...)
{
	const struct shell *sh = ctx;
	char buffer[512];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	shell_fprintf_normal(sh, "%s\n", buffer);
}

static void shell_error_adapter(void *ctx, const char *fmt, ...)
{
	const struct shell *sh = ctx;
	char buffer[256];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	shell_fprintf_error(sh, "%s\n", buffer);
}

static void shell_sleep_ms_adapter(int32_t durationMs)
{
	k_msleep(durationMs);
}

static const struct modem_shell_ops shellOps = {
	.modem_board_power_on = modem_board_power_on,
	.modem_board_power_off = modem_board_power_off,
	.modem_board_power_cycle = modem_board_power_cycle,
	.modem_board_reset_pulse = modem_board_reset_pulse,
	.modem_board_get_status = modem_board_get_status,
	.modem_at_send = modem_at_send,
	.sleep_ms = shell_sleep_ms_adapter,
	.print = shell_print_adapter,
	.error = shell_error_adapter,
};

static const struct shell *passthroughShell;
static bool passthroughActive;
static uint8_t passthroughTail;
static K_THREAD_STACK_DEFINE(passthroughStack, MODEM_PASSTHROUGH_STACK_SIZE);
static struct k_thread passthroughThread;
static bool passthroughThreadStarted;

static void modem_passthrough_stop(void)
{
	if (!passthroughActive) {
		return;
	}

	passthroughActive = false;
	shell_set_bypass(passthroughShell, NULL);
	shell_print(passthroughShell, "\r\n[modem passthrough disabled]");
	passthroughShell = NULL;
	passthroughTail = 0U;
}

static void modem_passthrough_trace_chunk(const struct shell *sh, const uint8_t *data, size_t length)
{
	char trace[MODEM_PASSTHROUGH_RX_TRACE_BUFFER_SIZE];
	size_t offset = 0U;

	for (size_t i = 0; i < length; ++i) {
		int written = snprintk(&trace[offset], sizeof(trace) - offset, "%02X", data[i]);
		if ((written <= 0) || ((size_t)written >= (sizeof(trace) - offset))) {
			break;
		}
		offset += (size_t)written;

		if ((i + 1U) < length) {
			if ((offset + 1U) >= sizeof(trace)) {
				break;
			}
			trace[offset++] = ' ';
			trace[offset] = '\0';
		}
	}

	shell_fprintf_normal(sh, "\r\n[modem rx %u] %s\r\n",
			     (unsigned int)length,
			     trace);
}

static void modem_passthrough_rx_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		if (!passthroughActive || (passthroughShell == NULL)) {
			k_msleep(20);
			continue;
		}

		uint8_t buffer[MODEM_PASSTHROUGH_RX_CHUNK_SIZE];
		size_t length = 0U;
		uint8_t byte;
		int ret = modem_at_uart_read_byte(&byte);

		if (ret == 0) {
			buffer[length++] = byte;

			int64_t deadline = k_uptime_get() + MODEM_PASSTHROUGH_RX_GATHER_WINDOW_MS;
			while (length < MODEM_PASSTHROUGH_RX_CHUNK_SIZE) {
				ret = modem_at_uart_read_byte(&byte);
				if (ret == 0) {
					buffer[length++] = byte;
					deadline = k_uptime_get() + MODEM_PASSTHROUGH_RX_GATHER_WINDOW_MS;
					continue;
				}

				if (ret != -1) {
					break;
				}

				if (k_uptime_get() >= deadline) {
					break;
				}

				k_msleep(1);
			}
		}

		if (length > 0U) {
			modem_passthrough_trace_chunk(passthroughShell, buffer, length);
			continue;
		}

		if (ret == -1) {
			k_msleep(10);
		} else {
			shell_error(passthroughShell, "\r\n[modem passthrough RX error: %d]", ret);
			modem_passthrough_stop();
		}
	}
}

static void modem_passthrough_bypass_cb(const struct shell *sh, uint8_t *data, size_t len)
{
	bool escape = false;

	if ((len == 0U) || !passthroughActive) {
		return;
	}

	if ((passthroughTail == MODEM_PASSTHROUGH_ESCAPE_PREFIX) &&
		(data[0] == MODEM_PASSTHROUGH_ESCAPE_SUFFIX)) {
		escape = true;
	} else {
		for (size_t i = 0; i + 1U < len; ++i) {
			if ((data[i] == MODEM_PASSTHROUGH_ESCAPE_PREFIX) &&
			    (data[i + 1U] == MODEM_PASSTHROUGH_ESCAPE_SUFFIX)) {
				escape = true;
				break;
			}
		}
	}

	if (escape) {
		modem_passthrough_stop();
		return;
	}

	passthroughTail = data[len - 1U];

	int ret = modem_at_uart_write(data, len);
	if (ret != 0) {
		shell_error(sh, "\r\n[modem passthrough TX error: %d]", ret);
		modem_passthrough_stop();
	}
}

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

static int cmd_modem_passthrough(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct modem_board_status st;
	int ret = modem_board_get_status(&st);
	if (ret != 0) {
		shell_error(sh, "status read failed: %d", ret);
		return ret;
	}

	if (st.rail_en != 1) {
		shell_error(sh, "modem is not powered");
		return -EHOSTDOWN;
	}

	if (!modem_at_uart_is_ready()) {
		shell_error(sh, "modem UART device is not ready");
		return -ENODEV;
	}

	if (passthroughActive) {
		shell_error(sh, "modem passthrough already active");
		return -EBUSY;
	}

	if (!passthroughThreadStarted) {
		k_thread_create(&passthroughThread,
				passthroughStack,
				K_THREAD_STACK_SIZEOF(passthroughStack),
				modem_passthrough_rx_thread,
				NULL,
				NULL,
				NULL,
				MODEM_PASSTHROUGH_THREAD_PRIORITY,
				0,
				K_NO_WAIT);
		passthroughThreadStarted = true;
	}

	passthroughShell = sh;
	passthroughTail = 0U;
	passthroughActive = true;
	shell_print(sh, "Entering modem UART passthrough. Press Ctrl-X then Ctrl-Q to exit.");
	shell_set_bypass(sh, modem_passthrough_bypass_cb);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_modem,
	SHELL_CMD(status, NULL, "Print modem GPIO status", cmd_modem_status),
	SHELL_CMD(reset, NULL, "Pulse modem reset (MODEM_nRST)", cmd_modem_reset),
	SHELL_CMD_ARG(power, NULL, "Modem power control: power <on|off|cycle>", cmd_modem_power, 2, 0),
	SHELL_CMD_ARG(at, NULL, "Send AT command: at <command>", cmd_modem_at, 2, 0),
	SHELL_CMD(passthrough, NULL,
		  "Raw UART passthrough to modem. Type directly; Ctrl-X then Ctrl-Q exits.",
		  cmd_modem_passthrough),
	SHELL_SUBCMD_SET_END /* Array terminator */
);

SHELL_CMD_REGISTER(modem, &sub_modem, "Modem control", NULL);
