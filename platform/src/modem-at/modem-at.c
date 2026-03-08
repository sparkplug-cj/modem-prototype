#include "modem-at.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#define MODEM_UART_NODE DT_NODELABEL(modem_uart)
#define MODEM_AT_RESPONSE_TIMEOUT_MS 5000
#define MODEM_AT_INTER_CHAR_TIMEOUT_MS 1000
#define MODEM_AT_PRE_SEND_FLUSH_MS 50

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
