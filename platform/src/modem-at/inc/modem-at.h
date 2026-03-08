#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum modem_at_exit_reason {
	MODEM_AT_EXIT_NONE = 0,
	MODEM_AT_EXIT_MATCH_OK,
	MODEM_AT_EXIT_MATCH_ERROR,
	MODEM_AT_EXIT_INTER_CHAR_TIMEOUT,
	MODEM_AT_EXIT_OVERALL_TIMEOUT,
	MODEM_AT_EXIT_BUFFER_FULL,
	MODEM_AT_EXIT_UART_ERROR,
};

struct modem_at_diagnostics {
	size_t bytesReceived;
	bool sawAnyByte;
	enum modem_at_exit_reason exitReason;
	int lastUartRet;
};

int modem_at_send(const char *command, char *response, size_t responseSize);
void modem_at_get_last_diagnostics(struct modem_at_diagnostics *diagnostics);
const char *modem_at_exit_reason_str(enum modem_at_exit_reason reason);

#ifdef __cplusplus
}
#endif
