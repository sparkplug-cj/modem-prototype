#include "modem-shell-core.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MODEM_AT_SYNC_RETRIES 3
#define MODEM_AT_BOOT_DELAY_MS 10000
#define MODEM_AT_SYNC_COMMAND "AT"
#define MODEM_AT_DISABLE_SLEEP_COMMAND "AT+KSLEEP=2"
#define MODEM_POST_DEFAULT_PAYLOAD "hello world"
#define MODEM_POST_MAX_PAYLOAD_BYTES 512

typedef int (*modem_shell_at_send_fn_t)(const char *command, char *response, size_t responseSize);

enum modem_shell_at_send_mode {
	MODEM_SHELL_AT_SEND_MODE_RUNTIME = 0,
	MODEM_SHELL_AT_SEND_MODE_POWER_ON,
};

static modem_shell_at_send_fn_t modem_shell_select_at_sender(const struct modem_shell_ops *ops,
					     enum modem_shell_at_send_mode mode)
{
	if ((mode == MODEM_SHELL_AT_SEND_MODE_POWER_ON) && (ops->modem_at_send_power_on != NULL)) {
		return ops->modem_at_send_power_on;
	}

	if (ops->modem_at_send_runtime != NULL) {
		return ops->modem_at_send_runtime;
	}

	return ops->modem_at_send;
}

static int modem_shell_run_at_command(const struct modem_shell_ops *ops,
				      enum modem_shell_at_send_mode mode,
				      const char *command,
				      char *response,
				      size_t responseSize)
{
	modem_shell_at_send_fn_t send_fn = modem_shell_select_at_sender(ops, mode);
	if (send_fn == NULL) {
		ops->error(ops->ctx,
			(mode == MODEM_SHELL_AT_SEND_MODE_POWER_ON) ?
				"no modem AT sender configured for power-on flow" :
				"no modem AT sender configured for runtime AT command");
		return -ENOSYS;
	}

	return send_fn(command, response, responseSize);
}

static char *modem_shell_trim_leading_spaces(char *text)
{
	while ((*text != '\0') && isspace((unsigned char)*text)) {
		text++;
	}

	return text;
}

static char *modem_shell_trim_matching_quotes(char *text)
{
	size_t len = strlen(text);

	if (len < 2U) {
		return text;
	}

	if (((text[0] == '"') && (text[len - 1U] == '"')) ||
	    ((text[0] == '\'') && (text[len - 1U] == '\''))) {
		text[len - 1U] = '\0';
		return text + 1;
	}

	return text;
}

static char *modem_shell_parse_at_command(size_t argc, char **argv, bool *debugEnabled)
{
	char *command;

	*debugEnabled = false;

	if (argc <= 1U) {
		return NULL;
	}

	if ((argc >= 2U) && (strcmp(argv[1], "--debug") == 0)) {
		*debugEnabled = true;
		if (argc <= 2U) {
			return NULL;
		}

		return argv[2];
	}

	if (argc > 2U) {
		return argv[1];
	}

	command = modem_shell_trim_leading_spaces(argv[1]);
	if (strncmp(command, "--debug", strlen("--debug")) == 0) {
		char next = command[strlen("--debug")];

		if ((next == '\0') || isspace((unsigned char)next)) {
			*debugEnabled = true;
			command = modem_shell_trim_leading_spaces(command + strlen("--debug"));
		}
	}

	if (*command == '\0') {
		return NULL;
	}

	command = modem_shell_trim_matching_quotes(command);
	command = modem_shell_trim_leading_spaces(command);

	return (*command == '\0') ? NULL : command;
}

static bool modem_shell_parse_port(const char *text)
{
	char *end = NULL;
	long value;

	if ((text == NULL) || (*text == '\0')) {
		return false;
	}

	errno = 0;
	value = strtol(text, &end, 10);
	if ((errno != 0) || (end == text) || (*end != '\0')) {
		return false;
	}

	return (value > 0L) && (value <= 65535L);
}

static const char *modem_shell_post_error_reason(int ret)
{
	switch (ret) {
	case -ENODEV:
		return "PPP interface not found";
	case -ETIMEDOUT:
		return "PPP link timeout (120s)";
	case -EHOSTUNREACH:
		return "DNS resolve failed or host unreachable";
	case -ECONNREFUSED:
		return "TLS connect or handshake failed";
	case -ENOTSUP:
		return "verified TLS is not configured (missing CA certificate)";
	case -EBADMSG:
		return "HTTP response has no status code";
	case -EIO:
		return "HTTP request failed or returned non-2xx status";
	default:
		return "unexpected transport error";
	}
}

static int modem_shell_disable_sleep_after_power_on(const struct modem_shell_ops *ops)
{
	char response[256];
	int ret = -ETIMEDOUT;

	ops->print(ops->ctx, "Waiting 10s for modem boot...");
	ops->sleep_ms(MODEM_AT_BOOT_DELAY_MS);

	for (int attempt = 0; attempt < MODEM_AT_SYNC_RETRIES; ++attempt) {
		ret = modem_shell_run_at_command(ops,
					       MODEM_SHELL_AT_SEND_MODE_POWER_ON,
					       MODEM_AT_SYNC_COMMAND,
					       response,
					       sizeof(response));
		if (ret == 0) {
			break;
		}
	}

	if (ret != 0) {
		ops->error(ops->ctx, "AT sync failed after power-on: %d", ret);
		return ret;
	}

	ops->print(ops->ctx, "Disabling sleep...");
	ret = modem_shell_run_at_command(ops,
					 MODEM_SHELL_AT_SEND_MODE_POWER_ON,
					 MODEM_AT_DISABLE_SLEEP_COMMAND,
					 response,
					 sizeof(response));
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
	struct modem_at_diagnostics diagnostics = {0};
	char response[256] = {0};
	bool debugRequested = false;
	bool debugOutputEnabled;
	const char *command;
	int ret;

	command = modem_shell_parse_at_command(argc, argv, &debugRequested);
	debugOutputEnabled = ops->modemAtDebug || debugRequested;
	if (command == NULL) {
		ops->error(ops->ctx, "usage: at [--debug] <command>");
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

	ret = modem_shell_run_at_command(ops,
					 MODEM_SHELL_AT_SEND_MODE_RUNTIME,
					 command,
					 response,
					 sizeof(response));
	modem_at_get_last_diagnostics(&diagnostics);
	if (ret != 0) {
		if (debugOutputEnabled && (response[0] != '\0')) {
			ops->error(ops->ctx,
				"[raw modem response on error]\n%s\n[modem-at] exit=%s bytes=%u ret=%d",
				response,
				modem_at_exit_reason_str(diagnostics.exitReason),
				(unsigned int)diagnostics.bytesReceived,
				ret);
		} else if (ret == -ETIMEDOUT) {
			if (debugOutputEnabled) {
				ops->error(ops->ctx,
					"AT command timed out waiting for modem response (exit=%s, bytes=%u)",
					modem_at_exit_reason_str(diagnostics.exitReason),
					(unsigned int)diagnostics.bytesReceived);
			} else {
				ops->error(ops->ctx, "AT command timed out waiting for modem response");
			}
		} else if (debugOutputEnabled) {
			ops->error(ops->ctx,
				"AT command failed: %d (exit=%s, bytes=%u)",
				ret,
				modem_at_exit_reason_str(diagnostics.exitReason),
				(unsigned int)diagnostics.bytesReceived);
		} else {
			ops->error(ops->ctx, "AT command failed: %d", ret);
		}
		return ret;
	}

	if (response[0] == '\0') {
		if (debugOutputEnabled) {
			ops->print(ops->ctx,
				"[empty modem response]\n[modem-at] exit=%s bytes=%u",
				modem_at_exit_reason_str(diagnostics.exitReason),
				(unsigned int)diagnostics.bytesReceived);
		} else {
			ops->print(ops->ctx, "[empty modem response]");
		}
		return 0;
	}

	if (debugOutputEnabled && (strcmp(response, command) == 0)) {
		ops->print(ops->ctx,
			"[echo only]\n%s\n[modem-at] exit=%s bytes=%u",
			response,
			modem_at_exit_reason_str(diagnostics.exitReason),
			(unsigned int)diagnostics.bytesReceived);
		return 0;
	}

	if (debugOutputEnabled) {
		ops->print(ops->ctx,
			"[raw modem response]\n%s\n[modem-at] exit=%s bytes=%u",
			response,
			modem_at_exit_reason_str(diagnostics.exitReason),
			(unsigned int)diagnostics.bytesReceived);
	} else {
		ops->print(ops->ctx, "%s", response);
	}
	return 0;
}

int modem_shell_cmd_post_core(const struct modem_shell_ops *ops, size_t argc, char **argv)
{
	struct modem_shell_http_post_request request;
	struct modem_shell_http_post_result result;
	char *payloadArg = NULL;
	int ret;

	if ((ops == NULL) || (ops->error == NULL) || (ops->print == NULL)) {
		return -EINVAL;
	}

	if (ops->https_post == NULL) {
		ops->error(ops->ctx, "HTTPS POST backend is not configured");
		return -ENOSYS;
	}

	if ((argc > 1U) && (strcmp(argv[1], "--insecure") == 0)) {
		ops->error(ops->ctx, "HTTPS POST always uses verified TLS; remove --insecure");
		return -EINVAL;
	}

	if (argc < 4U) {
		ops->error(ops->ctx,
			   "usage: post <host> <port> <path> [payload]");
		return -EINVAL;
	}

	if (!modem_shell_parse_port(argv[2])) {
		ops->error(ops->ctx, "invalid port: %s", argv[2]);
		return -EINVAL;
	}

	if ((argv[3] == NULL) || (argv[3][0] != '/')) {
		ops->error(ops->ctx, "path must start with '/'");
		return -EINVAL;
	}

	if (argc > 4U) {
		payloadArg = modem_shell_trim_matching_quotes(argv[4]);
		payloadArg = modem_shell_trim_leading_spaces(payloadArg);
	}

	if ((payloadArg == NULL) || (*payloadArg == '\0')) {
		payloadArg = (char *)MODEM_POST_DEFAULT_PAYLOAD;
	}

	if (strlen(payloadArg) > MODEM_POST_MAX_PAYLOAD_BYTES) {
		ops->error(ops->ctx,
			   "payload too large (max %u bytes, got %u)",
			   (unsigned int)MODEM_POST_MAX_PAYLOAD_BYTES,
			   (unsigned int)strlen(payloadArg));
		return -EINVAL;
	}

	memset(&request, 0, sizeof(request));
	request.host = argv[1];
	request.port = argv[2];
	request.path = argv[3];
	request.payload = (const uint8_t *)payloadArg;
	request.payloadLen = strlen(payloadArg);

	memset(&result, 0, sizeof(result));
	ret = ops->https_post(&request, &result);
	if (ret != 0) {
		if ((ret == -EHOSTUNREACH) &&
		    ((result.dnsResolveStatus != 0) || (result.dnsResolveErrno != 0))) {
			ops->error(ops->ctx,
			  "HTTPS POST failed: %d (%s; getaddrinfo=%d errno=%d dns_timeout_ms=%d)",
			  ret,
			  modem_shell_post_error_reason(ret),
			  result.dnsResolveStatus,
			  result.dnsResolveErrno,
			  result.dnsResolveTimeoutMs);
			return ret;
		}

		if (ret == -ENOTSUP) {
			ops->error(ops->ctx,
			   "HTTPS POST secure TLS is not configured (set CONFIG_CONTROL_TLS_SERVER_CA_CERT_PEM in control/prj.secrets.conf)");
			return ret;
		}

		ops->error(ops->ctx, "HTTPS POST failed: %d (%s)", ret,
			  modem_shell_post_error_reason(ret));
		return ret;
	}

	if (result.responsePreview[0] != '\0') {
		ops->print(ops->ctx,
			   "HTTPS POST OK status=%d bytes=%u preview=%s",
			   result.httpStatusCode,
			   (unsigned int)result.responseBytes,
			   result.responsePreview);
	} else {
		ops->print(ops->ctx,
			   "HTTPS POST OK status=%d bytes=%u",
			   result.httpStatusCode,
			   (unsigned int)result.responseBytes);
	}

	return 0;
}
