#include "modem-at.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#define MODEM_UART_NODE DT_NODELABEL(modem_uart)
#define MODEM_AT_RESPONSE_TIMEOUT_MS 5000
#define MODEM_AT_INTER_CHAR_TIMEOUT_MS 1000
#define MODEM_AT_IRQ_RX_CHUNK_SIZE 64
#define MODEM_AT_IRQ_RX_RING_SIZE 512

static const struct device *const modemUart = DEVICE_DT_GET(MODEM_UART_NODE);
static struct modem_at_diagnostics lastDiagnostics;
static uint8_t modemAtIrqRxBuf[MODEM_AT_IRQ_RX_RING_SIZE];
static struct ring_buf modemAtIrqRxRing;
static bool modemAtIrqConfigured;

static void modem_at_reset_last_diagnostics(void);

static void modem_at_uart_irq_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	uint8_t buf[MODEM_AT_IRQ_RX_CHUNK_SIZE];

	uart_irq_update(dev);

	/* RX: push any arrived bytes into the RX ring. */
	while (uart_irq_rx_ready(dev)) {
		int received = uart_fifo_read(dev, buf, sizeof(buf));

		if (received <= 0) {
			break;
		}

		(void)ring_buf_put(&modemAtIrqRxRing, buf, (uint32_t)received);
	}
}

static int modem_at_uart_irq_init_once(void)
{
	if (!device_is_ready(modemUart)) {
		return -ENODEV;
	}

	if (!modemAtIrqConfigured) {
		ring_buf_init(&modemAtIrqRxRing, sizeof(modemAtIrqRxBuf), modemAtIrqRxBuf);
		uart_irq_callback_user_data_set(modemUart, modem_at_uart_irq_cb, NULL);
		modemAtIrqConfigured = true;
	}

	return 0;
}

void modem_at_uart_irq_rx_enable(void)
{
	if (modem_at_uart_irq_init_once() != 0) {
		return;
	}

	ring_buf_reset(&modemAtIrqRxRing);
	uart_irq_rx_enable(modemUart);
}

void modem_at_uart_irq_rx_disable(void)
{
	uart_irq_rx_disable(modemUart);
	ring_buf_reset(&modemAtIrqRxRing);
}

uint32_t modem_at_uart_rx_read(uint8_t *buf, size_t size)
{
	return ring_buf_get(&modemAtIrqRxRing, buf, (uint32_t)size);
}

static int modem_at_irq_transport_open(void *ctx, char *response, size_t responseSize)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(response);
	ARG_UNUSED(responseSize);

	if (!modem_at_uart_is_ready()) {
		return -ENODEV;
	}

	modem_at_uart_irq_rx_enable();
	return 0;
}

static void modem_at_irq_transport_close(void *ctx)
{
	ARG_UNUSED(ctx);
	modem_at_uart_irq_rx_disable();
}

static uint32_t modem_at_irq_transport_read(void *ctx, uint8_t *buf, size_t size)
{
	ARG_UNUSED(ctx);
	return modem_at_uart_rx_read(buf, size);
}

static const struct modem_at_irq_transport modemAtInternalTransport = {
	.ctx = NULL,
	.open = modem_at_irq_transport_open,
	.close = modem_at_irq_transport_close,
	.read = modem_at_irq_transport_read,
};

static void modem_at_reset_last_diagnostics(void)
{
	lastDiagnostics.bytesReceived = 0U;
	lastDiagnostics.sawAnyByte = false;
	lastDiagnostics.exitReason = MODEM_AT_EXIT_NONE;
	lastDiagnostics.lastUartRet = 0;
}

void modem_at_get_last_diagnostics(struct modem_at_diagnostics *diagnostics)
{
	if (diagnostics == NULL) {
		return;
	}

	*diagnostics = lastDiagnostics;
}

const char *modem_at_exit_reason_str(enum modem_at_exit_reason reason)
{
	switch (reason) {
	case MODEM_AT_EXIT_NONE:
		return "none";
	case MODEM_AT_EXIT_MATCH_OK:
		return "matched-ok";
	case MODEM_AT_EXIT_MATCH_ERROR:
		return "matched-error";
	case MODEM_AT_EXIT_INTER_CHAR_TIMEOUT:
		return "inter-char-timeout";
	case MODEM_AT_EXIT_OVERALL_TIMEOUT:
		return "overall-timeout";
	case MODEM_AT_EXIT_BUFFER_FULL:
		return "buffer-full";
	case MODEM_AT_EXIT_UART_ERROR:
		return "uart-error";
	default:
		return "unknown";
	}
}

static void modem_at_irq_debug_log(const struct modem_at_irq_debug *debug, const char *fmt, ...)
{
	if ((debug == NULL) || (debug->log == NULL)) {
		return;
	}

	char buffer[256];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	debug->log(debug->ctx, "%s", buffer);
}

static int modem_at_irq_write_command(const char *command, const struct modem_at_irq_debug *debug)
{
	int ret = modem_at_uart_write((const uint8_t *)command, strlen(command));
	modem_at_irq_debug_log(debug, "[modem-at irq] command write ret=%d len=%u", ret,
			      (unsigned int)strlen(command));
	if (ret == 0) {
		static const uint8_t cr = '\r';
		ret = modem_at_uart_write(&cr, 1U);
		modem_at_irq_debug_log(debug, "[modem-at irq] CR write ret=%d", ret);
	}
	return ret;
}

static int modem_at_irq_collect_response(char *response,
					 size_t responseSize,
					 const struct modem_at_irq_transport *transport,
					 const struct modem_at_irq_debug *debug)
{
	size_t offset = 0U;
	bool loggedFirstRx = false;
	int64_t deadline = k_uptime_get() + MODEM_AT_RESPONSE_TIMEOUT_MS;

	for (;;) {
		uint8_t chunk[MODEM_AT_IRQ_RX_CHUNK_SIZE];
		uint32_t received = transport->read(transport->ctx, chunk, sizeof(chunk));
		if (received > 0U) {
			if (!loggedFirstRx) {
				modem_at_irq_debug_log(debug,
						"[modem-at irq] first rx chunk bytes=%u first=0x%02X",
						(unsigned int)received,
						(unsigned int)chunk[0]);
				loggedFirstRx = true;
			}
			for (uint32_t i = 0; i < received; ++i) {
				if (offset + 1U < responseSize) {
					response[offset++] = (char)chunk[i];
					response[offset] = '\0';
				}
			}

			if ((strstr(response, "\r\nOK\r\n") != NULL) ||
			    (strstr(response, "\nOK\r\n") != NULL) ||
			    (strstr(response, "\nOK\n") != NULL)) {
				modem_at_irq_debug_log(debug, "[modem-at irq] success bytes=%u",
						(unsigned int)offset);
				return 0;
			}
			if (strstr(response, "ERROR") != NULL) {
				modem_at_irq_debug_log(debug, "[modem-at irq] modem error bytes=%u",
						(unsigned int)offset);
				return -EIO;
			}

			deadline = k_uptime_get() + MODEM_AT_INTER_CHAR_TIMEOUT_MS;
			continue;
		}

		if (k_uptime_get() >= deadline) {
			modem_at_irq_debug_log(debug, "[modem-at irq] timeout bytes=%u", (unsigned int)offset);
			return -ETIMEDOUT;
		}

		k_msleep(10);
	}
}

bool modem_at_uart_is_ready(void)
{
	return device_is_ready(modemUart);
}

int modem_at_uart_write(const uint8_t *data, size_t length)
{
	if ((data == NULL) && (length > 0U)) {
		return -EINVAL;
	}

	if (!modem_at_uart_is_ready()) {
		return -ENODEV;
	}

	if (modem_at_uart_irq_init_once() != 0) {
		return -ENODEV;
	}

	/*
	 * Keep TX path simple and deterministic for AT commands.
	 * Commands are short, so poll-out is reliable here.
	 */
	for (size_t i = 0; i < length; ++i) {
		uart_poll_out(modemUart, data[i]);
	}

	return 0;
}

int modem_at_uart_read_byte(uint8_t *byte)
{
	if (byte == NULL) {
		return -EINVAL;
	}

	if (!modem_at_uart_is_ready()) {
		return -ENODEV;
	}

	return uart_poll_in(modemUart, byte);
}

int modem_at_send_irq(const char *command,
		     char *response,
		     size_t responseSize,
		     const struct modem_at_irq_transport *transport,
		     const struct modem_at_irq_debug *debug)
{
	modem_at_irq_debug_log(debug, "[modem-at irq] enter cmd='%s'", command != NULL ? command : "<null>");

	if ((command == NULL) || (response == NULL) || (responseSize == 0U) || (transport == NULL) ||
	    (transport->open == NULL) || (transport->close == NULL) || (transport->read == NULL)) {
		modem_at_irq_debug_log(debug, "[modem-at irq] invalid args");
		return -EINVAL;
	}

	int ret = transport->open(transport->ctx, response, responseSize);
	if (ret != 0) {
		modem_at_irq_debug_log(debug, "[modem-at irq] prepare/acquire failed ret=%d", ret);
		return ret;
	}

	modem_at_irq_debug_log(debug, "[modem-at irq] rx session acquired");

	ret = modem_at_irq_write_command(command, debug);
	if (ret != 0) {
		transport->close(transport->ctx);
		modem_at_irq_debug_log(debug, "[modem-at irq] write failed ret=%d", ret);
		return ret;
	}

	ret = modem_at_irq_collect_response(response, responseSize, transport, debug);
	transport->close(transport->ctx);
	return ret;
}

int modem_at_send(const char *command, char *response, size_t responseSize)
{
	int ret;

	modem_at_reset_last_diagnostics();
	ret = modem_at_send_irq(command, response, responseSize, &modemAtInternalTransport, NULL);
	if (ret == 0) {
		lastDiagnostics.exitReason = MODEM_AT_EXIT_MATCH_OK;
		lastDiagnostics.sawAnyByte = true;
		lastDiagnostics.bytesReceived = (response != NULL) ? strlen(response) : 0U;
	} else if (ret == -ETIMEDOUT) {
		lastDiagnostics.exitReason = MODEM_AT_EXIT_OVERALL_TIMEOUT;
	} else if (ret == -EIO) {
		lastDiagnostics.exitReason = MODEM_AT_EXIT_MATCH_ERROR;
		lastDiagnostics.sawAnyByte = true;
	}

	return ret;
}
