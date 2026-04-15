#include "modem-at.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#define MODEM_UART_NODE DT_NODELABEL(modem_uart)
#define MODEM_AT_RESPONSE_TIMEOUT_MS 5000
#define MODEM_AT_INTER_CHAR_TIMEOUT_MS 1000
#define MODEM_AT_PRE_SEND_FLUSH_MS 50
#define MODEM_AT_IRQ_RX_CHUNK_SIZE 64

static const struct device *const modemUart = DEVICE_DT_GET(MODEM_UART_NODE);
static struct modem_at_diagnostics lastDiagnostics;

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

static void modem_at_flush_rx(void)
{
	int64_t deadline = k_uptime_get() + MODEM_AT_PRE_SEND_FLUSH_MS;

	while (k_uptime_get() < deadline) {
		unsigned char ch;
		int ret = uart_poll_in(modemUart, &ch);
		if (ret != 0) {
			k_msleep(1);
		}
	}
}

static int response_append(char *response, size_t responseSize, size_t *length, unsigned char ch)
{
	if (*length + 1U >= responseSize) {
		lastDiagnostics.exitReason = MODEM_AT_EXIT_BUFFER_FULL;
		return -ENOSPC;
	}

	response[*length] = (char)ch;
	(*length)++;
	response[*length] = '\0';
	lastDiagnostics.bytesReceived = *length;
	return 0;
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

static int modem_at_irq_write_command(const char *command,
				      const struct modem_at_irq_transport *transport,
				      const struct modem_at_irq_debug *debug)
{
	int ret = transport->write(transport->ctx, (const uint8_t *)command, strlen(command));
	modem_at_irq_debug_log(debug, "[modem-at irq] command write ret=%d len=%u", ret,
			      (unsigned int)strlen(command));
	if (ret == 0) {
		static const uint8_t cr = '\r';
		ret = transport->write(transport->ctx, &cr, 1U);
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
	    (transport->open == NULL) || (transport->close == NULL) || (transport->write == NULL) ||
	    (transport->read == NULL)) {
		modem_at_irq_debug_log(debug, "[modem-at irq] invalid args");
		return -EINVAL;
	}

	int ret = transport->open(transport->ctx, response, responseSize);
	if (ret != 0) {
		modem_at_irq_debug_log(debug, "[modem-at irq] prepare/acquire failed ret=%d", ret);
		return ret;
	}

	modem_at_irq_debug_log(debug, "[modem-at irq] rx session acquired");

	ret = modem_at_irq_write_command(command, transport, debug);
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
	size_t length = 0U;
	int64_t deadline;
	int64_t interCharDeadline;
	bool sawAnyByte = false;

	if ((command == NULL) || (response == NULL) || (responseSize == 0U)) {
		return -EINVAL;
	}

	if (!device_is_ready(modemUart)) {
		return -ENODEV;
	}

	modem_at_reset_last_diagnostics();
	response[0] = '\0';
	modem_at_flush_rx();

	for (const char *p = command; *p != '\0'; ++p) {
		uart_poll_out(modemUart, (unsigned char)*p);
	}
	uart_poll_out(modemUart, '\r');

	deadline = k_uptime_get() + MODEM_AT_RESPONSE_TIMEOUT_MS;
	interCharDeadline = deadline;

	while (k_uptime_get() < deadline) {
		unsigned char ch;
		ret = uart_poll_in(modemUart, &ch);
		lastDiagnostics.lastUartRet = ret;
		if (ret == 0) {
			sawAnyByte = true;
			lastDiagnostics.sawAnyByte = true;
			interCharDeadline = k_uptime_get() + MODEM_AT_INTER_CHAR_TIMEOUT_MS;

			if (ch == '\r') {
				continue;
			}

			ret = response_append(response, responseSize, &length, ch);
			if (ret != 0) {
				return ret;
			}

			if (strstr(response, "\nOK\n") != NULL) {
				lastDiagnostics.exitReason = MODEM_AT_EXIT_MATCH_OK;
				break;
			}

			if (strstr(response, "\nERROR\n") != NULL) {
				lastDiagnostics.exitReason = MODEM_AT_EXIT_MATCH_ERROR;
				break;
			}
		} else if (ret == -1) {
			if (sawAnyByte && (k_uptime_get() >= interCharDeadline)) {
				lastDiagnostics.exitReason = MODEM_AT_EXIT_INTER_CHAR_TIMEOUT;
				break;
			}
			k_msleep(10);
		} else {
			lastDiagnostics.exitReason = MODEM_AT_EXIT_UART_ERROR;
			return -EIO;
		}
	}

	if (!sawAnyByte) {
		lastDiagnostics.exitReason = MODEM_AT_EXIT_OVERALL_TIMEOUT;
		return -ETIMEDOUT;
	}

	if (lastDiagnostics.exitReason == MODEM_AT_EXIT_NONE) {
		lastDiagnostics.exitReason = MODEM_AT_EXIT_OVERALL_TIMEOUT;
	}

	while ((length > 0U) && ((response[length - 1U] == '\n') || (response[length - 1U] == ' '))) {
		response[length - 1U] = '\0';
		length--;
	}

	while ((response[0] == '\n') || (response[0] == ' ')) {
		memmove(response, response + 1, strlen(response));
	}

	lastDiagnostics.bytesReceived = strlen(response);
	return 0;
}
