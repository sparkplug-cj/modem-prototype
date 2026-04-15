#pragma once

#include "modem-at.h"
#include "modem-board.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODEM_SHELL_HTTP_RESPONSE_PREVIEW_SIZE 128

struct modem_shell_http_post_request {
	const char *host;
	const char *port;
	const char *path;
	const uint8_t *payload;
	size_t payloadLen;
};

struct modem_shell_http_post_result {
	int httpStatusCode;
	int dnsResolveStatus;
	int dnsResolveErrno;
	int dnsResolveTimeoutMs;
	size_t responseBytes;
	char responsePreview[MODEM_SHELL_HTTP_RESPONSE_PREVIEW_SIZE];
};

struct modem_shell_ops {
	int (*modem_board_power_on)(void);
	int (*modem_board_power_off)(void);
	int (*modem_board_power_cycle)(void);
	int (*modem_board_reset_pulse)(void);
	int (*modem_board_get_status)(struct modem_board_status *out);
	int (*modem_at_send)(const char *command, char *response, size_t responseSize);
	int (*modem_at_send_runtime)(const char *command, char *response, size_t responseSize);
	int (*modem_at_send_power_on)(const char *command, char *response, size_t responseSize);
	void (*sleep_ms)(int32_t durationMs);
	void (*print)(void *ctx, const char *fmt, ...);
	void (*error)(void *ctx, const char *fmt, ...);
	void *ctx;
	bool modemAtDebug;
	int (*https_post)(const struct modem_shell_http_post_request *request,
			 struct modem_shell_http_post_result *result);
};

int modem_shell_cmd_status_core(const struct modem_shell_ops *ops);
int modem_shell_cmd_reset_core(const struct modem_shell_ops *ops);
int modem_shell_cmd_power_core(const struct modem_shell_ops *ops, size_t argc, char **argv);
int modem_shell_cmd_at_core(const struct modem_shell_ops *ops, size_t argc, char **argv);
int modem_shell_cmd_post_core(const struct modem_shell_ops *ops, size_t argc, char **argv);

#ifdef __cplusplus
}
#endif
