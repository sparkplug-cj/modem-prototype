#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

struct modem_at_irq_transport {
	void *ctx;
	int (*open)(void *ctx, char *response, size_t responseSize);
	void (*close)(void *ctx);
	uint32_t (*read)(void *ctx, uint8_t *buffer, size_t bufferSize);
};

struct modem_at_irq_debug {
	void *ctx;
	void (*log)(void *ctx, const char *fmt, ...);
};

int modem_at_send(const char *command, char *response, size_t responseSize);
int modem_at_send_irq(const char *command,
		     char *response,
		     size_t responseSize,
		     const struct modem_at_irq_transport *transport,
		     const struct modem_at_irq_debug *debug);
int modem_at_uart_write(const uint8_t *data, size_t length);
int modem_at_uart_read_byte(uint8_t *byte);
bool modem_at_uart_is_ready(void);
void modem_at_get_last_diagnostics(struct modem_at_diagnostics *diagnostics);
const char *modem_at_exit_reason_str(enum modem_at_exit_reason reason);

#ifdef __cplusplus
}
#endif
