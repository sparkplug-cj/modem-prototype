#include "modem-shell-core.h"

#include "modem-at.h"
#include "modem-board.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>

#define MODEM_PASSTHROUGH_STACK_SIZE 1024
#define MODEM_PASSTHROUGH_THREAD_PRIORITY 7
#define MODEM_PASSTHROUGH_ESCAPE_PREFIX 0x18u
#define MODEM_PASSTHROUGH_ESCAPE_SUFFIX 0x11u
#define MODEM_PASSTHROUGH_RX_CHUNK_SIZE 64
#define MODEM_PASSTHROUGH_RX_TRACE_BUFFER_SIZE (MODEM_PASSTHROUGH_RX_CHUNK_SIZE * 3U + 1U)
#define MODEM_PASSTHROUGH_IRQ_RING_SIZE 512

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

static const struct device *const modemUart = DEVICE_DT_GET(DT_NODELABEL(modem_uart));
static const struct shell *passthroughShell;
static bool passthroughActive;
static bool passthroughHexMode;
static uint8_t passthroughTail;
static K_THREAD_STACK_DEFINE(passthroughStack, MODEM_PASSTHROUGH_STACK_SIZE);
static struct k_thread passthroughThread;
static bool passthroughThreadStarted;
static uint8_t passthroughIrqRingBuffer[MODEM_PASSTHROUGH_IRQ_RING_SIZE];
static struct ring_buf passthroughIrqRing;
static bool passthroughIrqConfigured;

static void modem_passthrough_uart_irq_cb(const struct device *dev, void *user_data);

static int modem_uart_irq_prepare(void)
{
	if (!device_is_ready(modemUart)) {
		return -ENODEV;
	}

	if (!passthroughIrqConfigured) {
		ring_buf_init(&passthroughIrqRing,
			      sizeof(passthroughIrqRingBuffer),
			      passthroughIrqRingBuffer);
		uart_irq_callback_user_data_set(modemUart, modem_passthrough_uart_irq_cb, NULL);
		passthroughIrqConfigured = true;
	}

	return 0;
}

static int modem_at_send_power_on_irq(const char *command, char *response, size_t responseSize)
{
	int ret = modem_uart_irq_prepare();
	if (ret != 0) {
		return ret;
	}

	if ((command == NULL) || (response == NULL) || (responseSize == 0U)) {
		return -EINVAL;
	}

	ring_buf_reset(&passthroughIrqRing);
	response[0] = '\0';

	uart_irq_rx_enable(modemUart);

	ret = modem_at_uart_write((const uint8_t *)command, strlen(command));
	if (ret == 0) {
		static const uint8_t cr = '\r';
		ret = modem_at_uart_write(&cr, 1U);
	}
	if (ret != 0) {
		uart_irq_rx_disable(modemUart);
		return ret;
	}

	size_t offset = 0U;
	int64_t deadline = k_uptime_get() + 5000;
	for (;;) {
		uint8_t chunk[MODEM_PASSTHROUGH_RX_CHUNK_SIZE];
		uint32_t received = ring_buf_get(&passthroughIrqRing, chunk, sizeof(chunk));
		if (received > 0U) {
			for (uint32_t i = 0; i < received; ++i) {
				if (offset + 1U < responseSize) {
					response[offset++] = (char)chunk[i];
					response[offset] = '\0';
				}
			}

			if ((strstr(response, "\r\nOK\r\n") != NULL) ||
			    (strstr(response, "\nOK\r\n") != NULL) ||
			    (strstr(response, "\nOK\n") != NULL)) {
				uart_irq_rx_disable(modemUart);
				return 0;
			}
			if (strstr(response, "ERROR") != NULL) {
				uart_irq_rx_disable(modemUart);
				return -EIO;
			}

			deadline = k_uptime_get() + 1000;
			continue;
		}

		if (k_uptime_get() >= deadline) {
			uart_irq_rx_disable(modemUart);
			return -ETIMEDOUT;
		}

		k_msleep(10);
	}
}

static const struct modem_shell_ops shellOps = {
	.modem_board_power_on = modem_board_power_on,
	.modem_board_power_off = modem_board_power_off,
	.modem_board_power_cycle = modem_board_power_cycle,
	.modem_board_reset_pulse = modem_board_reset_pulse,
	.modem_board_get_status = modem_board_get_status,
	.modem_at_send = modem_at_send,
	.modem_at_send_power_on = modem_at_send_power_on_irq,
	.sleep_ms = shell_sleep_ms_adapter,
	.print = shell_print_adapter,
	.error = shell_error_adapter,
};

static void modem_passthrough_stop(void)
{
	if (!passthroughActive) {
		return;
	}

	passthroughActive = false;
	uart_irq_rx_disable(modemUart);
	shell_set_bypass(passthroughShell, NULL);
	shell_print(passthroughShell, "\r\n[modem passthrough disabled]");
	passthroughShell = NULL;
	passthroughTail = 0U;
	passthroughHexMode = false;
	ring_buf_reset(&passthroughIrqRing);
}

static void modem_passthrough_shell_write(const struct shell *sh, const char *data, size_t length)
{
	z_shell_print_stream(sh, data, length);
}

static void modem_passthrough_trace_chunk(const struct shell *sh, const uint8_t *data, size_t length)
{
	char trace[MODEM_PASSTHROUGH_RX_TRACE_BUFFER_SIZE];
	char line[MODEM_PASSTHROUGH_RX_TRACE_BUFFER_SIZE + 32U];
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

	int lineLen = snprintk(line, sizeof(line), "\r\n[modem rx %u] %s\r\n",
			       (unsigned int)length,
			       trace);
	if (lineLen > 0) {
		size_t writeLen = ((size_t)lineLen < (sizeof(line) - 1U)) ? (size_t)lineLen : (sizeof(line) - 1U);
		modem_passthrough_shell_write(sh, line, writeLen);
	}
}

static void modem_passthrough_print_text_chunk(const struct shell *sh, const uint8_t *data, size_t length)
{
	char text[MODEM_PASSTHROUGH_RX_CHUNK_SIZE * 4U + 1U];
	size_t offset = 0U;

	for (size_t i = 0; i < length; ++i) {
		uint8_t byte = data[i];

		if ((byte == '\r') || (byte == '\n') || (byte == '\t') ||
		    ((byte >= 0x20U) && (byte <= 0x7eU))) {
			if ((offset + 1U) >= sizeof(text)) {
				break;
			}
			text[offset++] = (char)byte;
			continue;
		}

		int written = snprintk(&text[offset], sizeof(text) - offset, "<%02X>", byte);
		if ((written <= 0) || ((size_t)written >= (sizeof(text) - offset))) {
			break;
		}
		offset += (size_t)written;
	}

	modem_passthrough_shell_write(sh, text, offset);
}

static void modem_passthrough_uart_irq_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uint8_t buffer[MODEM_PASSTHROUGH_RX_CHUNK_SIZE];

	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev)) {
		int received = uart_fifo_read(dev, buffer, sizeof(buffer));
		if (received <= 0) {
			break;
		}

		(void)ring_buf_put(&passthroughIrqRing, buffer, (uint32_t)received);
	}
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
		uint32_t length = ring_buf_get(&passthroughIrqRing, buffer, sizeof(buffer));
		if (length > 0U) {
			if (passthroughHexMode) {
				modem_passthrough_trace_chunk(passthroughShell, buffer, length);
			} else {
				modem_passthrough_print_text_chunk(passthroughShell, buffer, length);
			}
			continue;
		}

		k_msleep(10);
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
	bool hexMode = false;

	if (argc >= 2U) {
		if (strcmp(argv[1], "--hex") == 0) {
			hexMode = true;
		} else {
			shell_error(sh, "usage: modem passthrough [--hex]");
			return -EINVAL;
		}
	}

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

	if (!modem_at_uart_is_ready() || !device_is_ready(modemUart)) {
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

	ret = modem_uart_irq_prepare();
	if (ret != 0) {
		shell_error(sh, "failed to prepare modem UART IRQ RX: %d", ret);
		return ret;
	}

	ring_buf_reset(&passthroughIrqRing);
	passthroughShell = sh;
	passthroughTail = 0U;
	passthroughHexMode = hexMode;
	passthroughActive = true;
	uart_irq_rx_enable(modemUart);
	shell_print(sh,
		    hexMode ? "Entering modem UART passthrough (hex mode). Press Ctrl-X then Ctrl-Q to exit."
		            : "Entering modem UART passthrough. Press Ctrl-X then Ctrl-Q to exit.");
	shell_set_bypass(sh, modem_passthrough_bypass_cb);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_modem,
	SHELL_CMD(status, NULL, "Print modem GPIO status", cmd_modem_status),
	SHELL_CMD(reset, NULL, "Pulse modem reset (MODEM_nRST)", cmd_modem_reset),
	SHELL_CMD_ARG(power, NULL, "Modem power control: power <on|off|cycle>", cmd_modem_power, 2, 0),
	SHELL_CMD_ARG(at, NULL, "Send AT command: at <command>", cmd_modem_at, 2, 0),
	SHELL_CMD_ARG(passthrough, NULL,
		      "Raw UART passthrough to modem. Use --hex for RX trace mode; Ctrl-X then Ctrl-Q exits.",
		      cmd_modem_passthrough, 1, 1),
	SHELL_SUBCMD_SET_END /* Array terminator */
);

SHELL_CMD_REGISTER(modem, &sub_modem, "Modem control", NULL);
