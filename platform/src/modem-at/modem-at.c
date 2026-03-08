#include <modem-at.h>

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#define MODEM_UART_NODE DT_NODELABEL(modem_uart)
#define MODEM_AT_RESPONSE_TIMEOUT_MS 2000
#define MODEM_AT_INTER_CHAR_TIMEOUT_MS 100

static const struct device *const modemUart = DEVICE_DT_GET(MODEM_UART_NODE);

static int response_append(char *response, size_t responseSize, size_t *length, unsigned char ch)
{
	if (*length + 1U >= responseSize) {
		return -ENOSPC;
	}

	response[*length] = (char)ch;
	(*length)++;
	response[*length] = '\0';
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

	response[0] = '\0';

	for (const char *p = command; *p != '\0'; ++p) {
		uart_poll_out(modemUart, (unsigned char)*p);
	}
	uart_poll_out(modemUart, '\r');

	deadline = k_uptime_get() + MODEM_AT_RESPONSE_TIMEOUT_MS;
	interCharDeadline = deadline;

	while (k_uptime_get() < deadline) {
		unsigned char ch;
		ret = uart_poll_in(modemUart, &ch);
		if (ret == 0) {
			sawAnyByte = true;
			interCharDeadline = k_uptime_get() + MODEM_AT_INTER_CHAR_TIMEOUT_MS;

			if (ch == '\r') {
				continue;
			}

			ret = response_append(response, responseSize, &length, ch);
			if (ret != 0) {
				return ret;
			}

			if (strstr(response, "\nOK\n") != NULL) {
				char *ok = strstr(response, "\nOK\n");
				*ok = '\0';
				break;
			}

			if (strstr(response, "\nERROR\n") != NULL) {
				return -EIO;
			}
		} else if (ret == -1) {
			if (sawAnyByte && (k_uptime_get() >= interCharDeadline)) {
				break;
			}
			k_msleep(10);
		} else {
			return -EIO;
		}
	}

	if (!sawAnyByte) {
		return -ETIMEDOUT;
	}

	while ((length > 0U) && ((response[length - 1U] == '\n') || (response[length - 1U] == ' '))) {
		response[length - 1U] = '\0';
		length--;
	}

	while ((response[0] == '\n') || (response[0] == ' ')) {
		memmove(response, response + 1, strlen(response));
	}

	return 0;
}
