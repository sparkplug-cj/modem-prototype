#include "modem-shell-core.h"

#include "modem-at.h"
#include "modem-board.h"
#include "modem-link.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>

#define MODEM_PASSTHROUGH_STACK_SIZE 1024
#define MODEM_PASSTHROUGH_THREAD_PRIORITY 7
#define MODEM_PASSTHROUGH_ESCAPE_PREFIX 0x18u
#define MODEM_PASSTHROUGH_ESCAPE_SUFFIX 0x11u
#define MODEM_PASSTHROUGH_RX_CHUNK_SIZE 64
#define MODEM_PASSTHROUGH_RX_TRACE_BUFFER_SIZE (MODEM_PASSTHROUGH_RX_CHUNK_SIZE * 3U + 1U)
#define MODEM_UART_RX_RING_SIZE 512
#define MODEM_UART_TX_TIMEOUT_MS 1000
#define MODEM_HTTP_RECV_BUFFER_SIZE 512
#define MODEM_HTTP_REQUEST_TIMEOUT_MS 30000
#define MODEM_HTTP_TLS_SEC_TAG 4242
#define MODEM_HTTP_TLS_CA_CERT_BUFFER_SIZE (sizeof(CONFIG_CONTROL_TLS_SERVER_CA_CERT_PEM) + 1U)

LOG_MODULE_REGISTER(modem_shell, CONFIG_LOG_DEFAULT_LEVEL);

struct modem_http_response_context {
	struct modem_shell_http_post_result *result;
	size_t previewLength;
};

static void modem_shell_print_link_diagnostics(const struct shell *sh,
				       const struct modem_link_diagnostics *diagnostics);

static char modemHttpCaCertificate[MODEM_HTTP_TLS_CA_CERT_BUFFER_SIZE];
static bool modemHttpTlsCredentialAttempted;
static int modemHttpTlsCredentialStatus = -ENOTSUP;

static char modem_http_preview_char(uint8_t byte)
{
	if ((byte >= 0x20U) && (byte <= 0x7eU)) {
		return (char)byte;
	}

	if ((byte == '\r') || (byte == '\n') || (byte == '\t')) {
		return (char)byte;
	}

	return '.';
}

static int modem_http_response_cb(struct http_response *rsp,
				       enum http_final_call finalData,
				       void *userData)
{
	struct modem_http_response_context *context = userData;
	const uint8_t *body;
	size_t remaining;
	size_t chunkLength;

	ARG_UNUSED(finalData);

	if ((rsp == NULL) || (context == NULL) || (context->result == NULL)) {
		return 0;
	}

	context->result->httpStatusCode = rsp->http_status_code;
	body = rsp->body_frag_start;
	chunkLength = rsp->body_frag_len;
	context->result->responseBytes += chunkLength;

	if ((body == NULL) || (chunkLength == 0U)) {
		return 0;
	}

	remaining = (sizeof(context->result->responsePreview) - 1U) - context->previewLength;
	if (remaining == 0U) {
		return 0;
	}

	if (chunkLength > remaining) {
		chunkLength = remaining;
	}

	for (size_t i = 0; i < chunkLength; ++i) {
		context->result->responsePreview[context->previewLength + i] =
			modem_http_preview_char(body[i]);
	}

	context->previewLength += chunkLength;
	context->result->responsePreview[context->previewLength] = '\0';
	return 0;
}

static int modem_http_normalize_pem(char *buffer, size_t bufferSize, const char *encodedPem)
{
	size_t readIndex = 0U;
	size_t writeIndex = 0U;

	if ((buffer == NULL) || (bufferSize == 0U) || (encodedPem == NULL)) {
		return -EINVAL;
	}

	while (encodedPem[readIndex] != '\0') {
		char output = encodedPem[readIndex];

		if (encodedPem[readIndex] == '\\') {
			size_t slashIndex = readIndex;

			while (encodedPem[slashIndex] == '\\') {
				slashIndex++;
			}

			switch (encodedPem[slashIndex]) {
			case 'n':
				output = '\n';
				readIndex = slashIndex + 1U;
				break;
			case 'r':
				output = '\r';
				readIndex = slashIndex + 1U;
				break;
			case 't':
				output = '\t';
				readIndex = slashIndex + 1U;
				break;
			default:
				readIndex++;
				break;
			}
		} else {
			readIndex++;
		}

		if ((writeIndex + 1U) >= bufferSize) {
			return -ENOMEM;
		}

		buffer[writeIndex++] = output;
	}

	buffer[writeIndex] = '\0';
	return 0;
}

static int modem_http_register_ca_certificate(void)
{
	int ret;

	if (modemHttpTlsCredentialAttempted) {
		return modemHttpTlsCredentialStatus;
	}

	modemHttpTlsCredentialAttempted = true;

	if (CONFIG_CONTROL_TLS_SERVER_CA_CERT_PEM[0] == '\0') {
		modemHttpTlsCredentialStatus = -ENOTSUP;
		return modemHttpTlsCredentialStatus;
	}

	ret = modem_http_normalize_pem(modemHttpCaCertificate,
					      sizeof(modemHttpCaCertificate),
					      CONFIG_CONTROL_TLS_SERVER_CA_CERT_PEM);
	if (ret != 0) {
		modemHttpTlsCredentialStatus = ret;
		return ret;
	}

	ret = tls_credential_add(MODEM_HTTP_TLS_SEC_TAG,
				 TLS_CREDENTIAL_CA_CERTIFICATE,
				 modemHttpCaCertificate,
				 strlen(modemHttpCaCertificate) + 1U);
	if ((ret == 0) || (ret == -EEXIST)) {
		modemHttpTlsCredentialStatus = 0;
		return 0;
	}

	modemHttpTlsCredentialStatus = ret;
	return ret;
}

static int modem_http_connect_socket(const struct modem_shell_http_post_request *request,
				      struct modem_shell_http_post_result *result,
				      struct zsock_addrinfo **addressOut)
{
	struct zsock_addrinfo hints = {0};
	struct zsock_addrinfo *address = NULL;
	sec_tag_t secTagList[] = { MODEM_HTTP_TLS_SEC_TAG };
	int socketFd = -1;
	int verifyMode = TLS_PEER_VERIFY_REQUIRED;
	int ret;

	ret = modem_http_register_ca_certificate();
	if (ret != 0) {
		return ret;
	}

	if (result != NULL) {
		result->dnsResolveTimeoutMs = CONFIG_NET_SOCKETS_DNS_TIMEOUT;
	}

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	LOG_INF("HTTPS connect start host=%s port=%s dns_timeout_ms=%d",
		request->host,
		request->port,
		CONFIG_NET_SOCKETS_DNS_TIMEOUT);

	ret = zsock_getaddrinfo(request->host, request->port, &hints, &address);
	if ((ret != 0) || (address == NULL)) {
		if (result != NULL) {
			result->dnsResolveStatus = ret;
			result->dnsResolveErrno = errno;
		}
		LOG_WRN("HTTPS connect resolve failed host=%s status=%d errno=%d",
			request->host,
			ret,
			errno);
		return -EHOSTUNREACH;
	}

	LOG_INF("HTTPS connect resolved host=%s", request->host);

	for (struct zsock_addrinfo *candidate = address; candidate != NULL; candidate = candidate->ai_next) {
		socketFd = zsock_socket(candidate->ai_family, candidate->ai_socktype,
					      IPPROTO_TLS_1_2);
		if (socketFd < 0) {
			continue;
		}

		ret = zsock_setsockopt(socketFd, SOL_TLS, TLS_HOSTNAME,
					       request->host, strlen(request->host));
		if (ret < 0) {
			zsock_close(socketFd);
			socketFd = -1;
			continue;
		}

		ret = zsock_setsockopt(socketFd, SOL_TLS, TLS_SEC_TAG_LIST,
					       secTagList, sizeof(secTagList));
		if (ret < 0) {
			zsock_close(socketFd);
			socketFd = -1;
			continue;
		}

		ret = zsock_setsockopt(socketFd, SOL_TLS, TLS_PEER_VERIFY,
					       &verifyMode, sizeof(verifyMode));
		if (ret < 0) {
			zsock_close(socketFd);
			socketFd = -1;
			continue;
		}

		ret = zsock_connect(socketFd, candidate->ai_addr, candidate->ai_addrlen);
		if (ret == 0) {
			LOG_INF("HTTPS connect established host=%s port=%s", request->host, request->port);
			*addressOut = address;
			return socketFd;
		}

		zsock_close(socketFd);
		socketFd = -1;
	}

	zsock_freeaddrinfo(address);
	LOG_WRN("HTTPS connect failed after resolve host=%s port=%s", request->host, request->port);
	return -ECONNREFUSED;
}

static int modem_shell_dns_lookup(const char *host,
				  char *resolvedAddress,
				  size_t resolvedAddressSize,
				  int *dnsStatusOut,
				  int *dnsErrnoOut)
{
	struct zsock_addrinfo hints = {0};
	struct zsock_addrinfo *address = NULL;
	const char *ntopResult;
	int ret;

	if ((host == NULL) || (resolvedAddress == NULL) || (resolvedAddressSize == 0U)) {
		return -EINVAL;
	}

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	LOG_INF("DNS lookup start host=%s timeout_ms=%d", host, CONFIG_NET_SOCKETS_DNS_TIMEOUT);

	ret = zsock_getaddrinfo(host, NULL, &hints, &address);
	if (dnsStatusOut != NULL) {
		*dnsStatusOut = ret;
	}

	if (dnsErrnoOut != NULL) {
		*dnsErrnoOut = errno;
	}

	if ((ret != 0) || (address == NULL) || (address->ai_addr == NULL) ||
	    (address->ai_family != AF_INET)) {
		LOG_WRN("DNS lookup failed host=%s status=%d errno=%d",
			host,
			ret,
			errno);
		if (address != NULL) {
			zsock_freeaddrinfo(address);
		}

		return -EHOSTUNREACH;
	}

	ntopResult = zsock_inet_ntop(AF_INET,
				    &((struct sockaddr_in *)address->ai_addr)->sin_addr,
				    resolvedAddress,
				    resolvedAddressSize);
	zsock_freeaddrinfo(address);
	if (ntopResult == NULL) {
		LOG_WRN("DNS lookup ntop failed host=%s errno=%d", host, errno);
		return -EIO;
	}

	LOG_INF("DNS lookup success host=%s addr=%s", host, resolvedAddress);

	return 0;
}

static int modem_shell_bind_static_dns_servers(struct net_if *pppIface)
{
#if defined(CONFIG_DNS_SERVER_IP_ADDRESSES)
	struct dns_resolve_context *dnsContext;
	int interfaceIndex;
	int ret;
	const char *dnsServer2Text;
	const char *dnsServers[] = {
		CONFIG_DNS_SERVER1,
#if defined(CONFIG_DNS_SERVER2)
		CONFIG_DNS_SERVER2,
#endif
		NULL,
	};
	int interfaces[] = { 0, 0 };

	if (pppIface == NULL) {
		return -EINVAL;
	}

	interfaceIndex = net_if_get_by_iface(pppIface);
	if (interfaceIndex <= 0) {
		return -ENODEV;
	}

#if defined(CONFIG_DNS_SERVER2)
	dnsServer2Text = CONFIG_DNS_SERVER2;
#else
	dnsServer2Text = "<none>";
#endif

	LOG_INF("DNS fallback bind start ifindex=%d dns1=%s dns2=%s",
		interfaceIndex,
		CONFIG_DNS_SERVER1,
		dnsServer2Text
	);

	interfaces[0] = interfaceIndex;
	interfaces[1] = interfaceIndex;

	dnsContext = dns_resolve_get_default();
	if (dnsContext == NULL) {
		return -ENODEV;
	}

	(void)dns_resolve_remove_source(dnsContext, interfaceIndex, DNS_SOURCE_MANUAL);

	ret = dns_resolve_reconfigure_with_interfaces(dnsContext,
					      dnsServers,
					      NULL,
					      interfaces,
					      DNS_SOURCE_MANUAL);
	if (ret == 0) {
		LOG_INF("DNS fallback bind ok ifindex=%d", interfaceIndex);
	} else {
		LOG_WRN("DNS fallback bind failed ifindex=%d ret=%d", interfaceIndex, ret);
	}

	return ret;
#else
	ARG_UNUSED(pppIface);
	return 0;
#endif
}

static bool modem_shell_can_use_static_dns_fallback(int ret,
					    const struct modem_link_diagnostics *diagnostics)
{
#if defined(CONFIG_DNS_SERVER_IP_ADDRESSES)
	if (diagnostics == NULL) {
		return false;
	}

	if (ret != -ETIMEDOUT) {
		return false;
	}

	return diagnostics->stage == MODEM_LINK_STAGE_L4_CONNECTED;
#else
	ARG_UNUSED(ret);
	ARG_UNUSED(diagnostics);
	return false;
#endif
}

static int modem_shell_ensure_ppp_ready(const struct shell *sh)
{
	struct modem_link_options options = modem_link_default_options();
	struct modem_link_diagnostics diagnostics;
	struct net_if *pppIface = NULL;
	int interfaceIndex = 0;
	int fallbackRet = -ENODEV;
	int ret;

	LOG_INF("PPP ready check start");

	ret = modem_link_get_ppp_iface(&pppIface);
	if ((ret == 0) && (pppIface != NULL) && net_if_is_up(pppIface)) {
		interfaceIndex = net_if_get_by_iface(pppIface);
		LOG_INF("PPP already up ifindex=%d; refreshing DNS fallback", interfaceIndex);
		fallbackRet = modem_shell_bind_static_dns_servers(pppIface);
		if (fallbackRet != 0) {
			LOG_WRN("PPP already up fallback bind failed ifindex=%d ret=%d",
				interfaceIndex,
				fallbackRet);
		}
		return 0;
	}

	ret = modem_link_ensure_ready(&options, &diagnostics);
	if (ret != 0) {
		if (modem_shell_can_use_static_dns_fallback(ret, &diagnostics)) {
			ret = modem_link_get_ppp_iface(&pppIface);
			if ((ret == 0) && (pppIface != NULL)) {
				interfaceIndex = net_if_get_by_iface(pppIface);
				LOG_INF("PPP reached L4 without DNS event; using fallback ifindex=%d",
					interfaceIndex);
				fallbackRet = modem_shell_bind_static_dns_servers(pppIface);
				if (fallbackRet == 0) {
					shell_print(sh,
						    "PPP DNS not published, using static DNS fallback");
					return 0;
				}

				shell_error(sh, "Static DNS fallback failed: %d", fallbackRet);
			}

			ret = (ret != 0) ? ret : fallbackRet;
		}

		modem_shell_print_link_diagnostics(sh, &diagnostics);
		shell_error(sh, "PPP bring-up failed: %d", ret);
		return ret;
	}

	LOG_INF("PPP ready check complete via bring-up");

	return 0;
}

static int modem_shell_https_post(const struct modem_shell_http_post_request *request,
				  struct modem_shell_http_post_result *result)
{
	static const char *const optionalHeaders[] = {
		"Connection: close\r\n",
		NULL,
	};
	uint8_t recvBuffer[MODEM_HTTP_RECV_BUFFER_SIZE] = {0};
	struct modem_http_response_context responseContext = {
		.result = result,
		.previewLength = 0U,
	};
	struct net_if *pppIface = NULL;
	struct http_request httpRequest = {0};
	struct zsock_addrinfo *address = NULL;
	int socketFd;
	int ret;

	if ((request == NULL) || (result == NULL)) {
		return -EINVAL;
	}

	ret = modem_link_get_ppp_iface(&pppIface);
	if (ret != 0) {
		return ret;
	}

	if ((pppIface == NULL) || !net_if_is_up(pppIface)) {
		return -ETIMEDOUT;
	}

	memset(result, 0, sizeof(*result));

	socketFd = modem_http_connect_socket(request, result, &address);
	if (socketFd < 0) {
		return socketFd;
	}

	httpRequest.method = HTTP_POST;
	httpRequest.url = request->path;
	httpRequest.protocol = "HTTP/1.1";
	httpRequest.host = request->host;
	httpRequest.port = request->port;
	httpRequest.response = modem_http_response_cb;
	httpRequest.recv_buf = recvBuffer;
	httpRequest.recv_buf_len = sizeof(recvBuffer);
	httpRequest.content_type_value = "text/plain";
	httpRequest.payload = (const char *)request->payload;
	httpRequest.payload_len = request->payloadLen;
	httpRequest.optional_headers = (const char **)optionalHeaders;

	ret = http_client_req(socketFd, &httpRequest, MODEM_HTTP_REQUEST_TIMEOUT_MS,
			      &responseContext);
	zsock_close(socketFd);
	zsock_freeaddrinfo(address);
	if (ret < 0) {
		if ((ret == -ECONNRESET) || (ret == -ECONNABORTED)) {
			return -ECONNREFUSED;
		}

		return ret;
	}

	if (result->httpStatusCode == 0) {
		return -EBADMSG;
	}

	if ((result->httpStatusCode < 200) || (result->httpStatusCode > 299)) {
		return -EIO;
	}

	return 0;
}

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
static bool passthroughDebugMode;
static uint8_t passthroughTail;
static K_THREAD_STACK_DEFINE(passthroughStack, MODEM_PASSTHROUGH_STACK_SIZE);
static struct k_thread passthroughThread;
static bool passthroughThreadStarted;
static uint8_t modemUartRxRingBuffer[MODEM_UART_RX_RING_SIZE];
static struct ring_buf modemUartRxRing;
static bool modemUartRxIrqConfigured;
static struct k_sem modemUartTxDone;
static bool modemUartTxDoneInitialized;
static const struct shell *modemAtDebugShell;

struct modem_uart_tx_state {
	const uint8_t *buffer;
	size_t length;
	size_t offset;
	int result;
	bool active;
};

static struct modem_uart_tx_state modemUartTx;

enum modem_uart_rx_owner {
	MODEM_UART_RX_OWNER_NONE = 0,
	MODEM_UART_RX_OWNER_AT,
	MODEM_UART_RX_OWNER_PASSTHROUGH,
};

static enum modem_uart_rx_owner modemUartRxOwner;

static void modem_uart_rx_irq_cb(const struct device *dev, void *user_data);

static int modem_uart_rx_prepare(void)
{
	if (!device_is_ready(modemUart)) {
		return -ENODEV;
	}

	if (!modemUartRxIrqConfigured) {
		ring_buf_init(&modemUartRxRing,
			      sizeof(modemUartRxRingBuffer),
			      modemUartRxRingBuffer);
		uart_irq_callback_user_data_set(modemUart, modem_uart_rx_irq_cb, NULL);
		modemUartRxIrqConfigured = true;
	}

	if (!modemUartTxDoneInitialized) {
		k_sem_init(&modemUartTxDone, 0, 1);
		modemUartTxDoneInitialized = true;
	}

	return 0;
}

static int modem_uart_rx_acquire(enum modem_uart_rx_owner owner)
{
	int ret = modem_uart_rx_prepare();
	if (ret != 0) {
		return ret;
	}

	if (modemUartRxOwner != MODEM_UART_RX_OWNER_NONE) {
		return -EBUSY;
	}

	modemUartRxOwner = owner;
	ring_buf_reset(&modemUartRxRing);
	uart_irq_rx_enable(modemUart);
	return 0;
}

static void modem_uart_rx_release(enum modem_uart_rx_owner owner)
{
	if (modemUartRxOwner != owner) {
		return;
	}

	uart_irq_rx_disable(modemUart);
	ring_buf_reset(&modemUartRxRing);
	modemUartRxOwner = MODEM_UART_RX_OWNER_NONE;
}

static uint32_t modem_uart_rx_read(void *ctx, uint8_t *buffer, size_t bufferSize)
{
	ARG_UNUSED(ctx);
	return ring_buf_get(&modemUartRxRing, buffer, bufferSize);
}

static int modem_uart_irq_write(void *ctx, const uint8_t *data, size_t length)
{
	ARG_UNUSED(ctx);

	if ((data == NULL) && (length > 0U)) {
		return -EINVAL;
	}

	if (length == 0U) {
		return 0;
	}

	int ret = modem_uart_rx_prepare();
	if (ret != 0) {
		return ret;
	}

	if (modemUartRxOwner == MODEM_UART_RX_OWNER_NONE) {
		return -EACCES;
	}

	unsigned int key = irq_lock();
	if (modemUartTx.active) {
		irq_unlock(key);
		return -EBUSY;
	}

	modemUartTx.buffer = data;
	modemUartTx.length = length;
	modemUartTx.offset = 0U;
	modemUartTx.result = 0;
	modemUartTx.active = true;
	irq_unlock(key);

	uart_irq_tx_enable(modemUart);
	ret = k_sem_take(&modemUartTxDone, K_MSEC(MODEM_UART_TX_TIMEOUT_MS));
	if (ret == 0) {
		return modemUartTx.result;
	}

	uart_irq_tx_disable(modemUart);
	key = irq_lock();
	modemUartTx.active = false;
	irq_unlock(key);
	return -ETIMEDOUT;
}

static int modem_uart_irq_at_session_open(void *ctx, char *response, size_t responseSize)
{
	ARG_UNUSED(ctx);

	if ((response == NULL) || (responseSize == 0U)) {
		return -EINVAL;
	}

	int ret = modem_uart_rx_acquire(MODEM_UART_RX_OWNER_AT);
	if (ret != 0) {
		return ret;
	}

	response[0] = '\0';
	return 0;
}

static void modem_uart_irq_at_session_close(void *ctx)
{
	ARG_UNUSED(ctx);
	modem_uart_rx_release(MODEM_UART_RX_OWNER_AT);
}

static void modem_at_debug_log_adapter(void *ctx, const char *fmt, ...)
{
	ARG_UNUSED(ctx);

	if (modemAtDebugShell == NULL) {
		return;
	}

	char buffer[256];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	shell_fprintf_normal(modemAtDebugShell, "%s\n", buffer);
}

static bool modem_shell_at_debug_requested(size_t argc, char **argv)
{
	char *raw;

	if (argc <= 1U) {
		return false;
	}

	if (strcmp(argv[1], "--debug") == 0) {
		return true;
	}

	if (argc != 2U) {
		return false;
	}

	raw = argv[1];
	while ((*raw != '\0') && isspace((unsigned char)*raw)) {
		raw++;
	}

	if (strncmp(raw, "--debug", strlen("--debug")) != 0) {
		return false;
	}

	return (raw[strlen("--debug")] == '\0') ||
	       isspace((unsigned char)raw[strlen("--debug")]);
}

static int modem_shell_at_send_irq(const char *command, char *response, size_t responseSize)
{
	static const struct modem_at_irq_transport transport = {
		.ctx = NULL,
		.open = modem_uart_irq_at_session_open,
		.close = modem_uart_irq_at_session_close,
		.write = modem_uart_irq_write,
		.read = modem_uart_rx_read,
	};
	const struct modem_at_irq_debug debug = {
		.ctx = NULL,
		.log = modem_at_debug_log_adapter,
	};

	return modem_at_send_irq(command, response, responseSize, &transport, &debug);
}

static const struct modem_shell_ops shellOps = {
	// .modem_board_power_on = modem_board_power_on,
	.modem_board_power_off = modem_board_power_off,
	.modem_board_power_cycle = modem_board_power_cycle,
	.modem_board_reset_pulse = modem_board_reset_pulse,
	.modem_board_get_status = modem_board_get_status,
	.modem_at_send = modem_at_send,
	.modem_at_send_runtime = modem_shell_at_send_irq,
	.modem_at_send_power_on = modem_shell_at_send_irq,
	.sleep_ms = shell_sleep_ms_adapter,
	.print = shell_print_adapter,
	.error = shell_error_adapter,
	.modemAtDebug = false,
	.https_post = modem_shell_https_post,
};

static void modem_shell_print_link_diagnostics(const struct shell *sh,
				       const struct modem_link_diagnostics *diagnostics)
{
	shell_print(sh,
		    "stage=%s error=%d event=%llu resumed=%u iface=%u up=%u l4=%u dns=%u",
		    modem_link_stage_str(diagnostics->stage),
		    diagnostics->lastError,
		    (unsigned long long)diagnostics->lastEvent,
		    diagnostics->modemResumed ? 1U : 0U,
		    diagnostics->pppInterfaceFound ? 1U : 0U,
		    diagnostics->pppInterfaceUp ? 1U : 0U,
		    diagnostics->l4Connected ? 1U : 0U,
		    diagnostics->dnsServerAdded ? 1U : 0U);
}

static void modem_passthrough_stop(void)
{
	if (!passthroughActive) {
		return;
	}

	passthroughActive = false;
	modem_uart_rx_release(MODEM_UART_RX_OWNER_PASSTHROUGH);
	shell_set_bypass(passthroughShell, NULL);
	shell_print(passthroughShell, "\r\n[modem passthrough disabled]");
	passthroughShell = NULL;
	passthroughTail = 0U;
	passthroughDebugMode = false;
	ring_buf_reset(&modemUartRxRing);
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

static void modem_uart_rx_irq_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uint8_t buffer[MODEM_PASSTHROUGH_RX_CHUNK_SIZE];

	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev)) {
		int received = uart_fifo_read(dev, buffer, sizeof(buffer));
		if (received <= 0) {
			break;
		}

		(void)ring_buf_put(&modemUartRxRing, buffer, (uint32_t)received);
	}

	while (uart_irq_tx_ready(dev)) {
		if (!modemUartTx.active) {
			uart_irq_tx_disable(dev);
			break;
		}

		size_t remaining = modemUartTx.length - modemUartTx.offset;
		if (remaining == 0U) {
			modemUartTx.result = 0;
			modemUartTx.active = false;
			uart_irq_tx_disable(dev);
			k_sem_give(&modemUartTxDone);
			break;
		}

		int chunk = (remaining > (size_t)INT_MAX) ? INT_MAX : (int)remaining;
		int written = uart_fifo_fill(dev, &modemUartTx.buffer[modemUartTx.offset], chunk);
		if (written < 0) {
			modemUartTx.result = written;
			modemUartTx.active = false;
			uart_irq_tx_disable(dev);
			k_sem_give(&modemUartTxDone);
			break;
		}

		if (written == 0) {
			break;
		}

		modemUartTx.offset += (size_t)written;
		if (modemUartTx.offset >= modemUartTx.length) {
			modemUartTx.result = 0;
			modemUartTx.active = false;
			uart_irq_tx_disable(dev);
			k_sem_give(&modemUartTxDone);
			break;
		}
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
		uint32_t length = ring_buf_get(&modemUartRxRing, buffer, sizeof(buffer));
		if (length > 0U) {
			if (passthroughDebugMode) {
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

	int ret = modem_uart_irq_write(NULL, data, len);
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

// static int cmd_modem_power(const struct shell *sh, size_t argc, char **argv)
// {
// 	if ((argc >= 2U) &&
// 	    ((strcmp(argv[1], "on") == 0) || (strcmp(argv[1], "cycle") == 0)) &&
// 	    (modemUartRxOwner != MODEM_UART_RX_OWNER_NONE)) {
// 		shell_error(sh, "modem UART RX is busy");
// 		return -EBUSY;
// 	}

// 	struct modem_shell_ops ops = shellOps;
// 	ops.ctx = (void *)sh;
// 	return modem_shell_cmd_power_core(&ops, argc, argv);
// }

static int cmd_modem_at(const struct shell *sh, size_t argc, char **argv)
{
	if (modemUartRxOwner != MODEM_UART_RX_OWNER_NONE) {
		shell_error(sh, "modem UART RX is busy");
		return -EBUSY;
	}

	struct modem_shell_ops ops = shellOps;
	ops.ctx = (void *)sh;
	ops.modemAtDebug = modem_shell_at_debug_requested(argc, argv);
	modemAtDebugShell = ops.modemAtDebug ? sh : NULL;
	int ret = modem_shell_cmd_at_core(&ops, argc, argv);
	modemAtDebugShell = NULL;
	return ret;
}

static int cmd_modem_post(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (modemUartRxOwner != MODEM_UART_RX_OWNER_NONE) {
		shell_error(sh, "modem UART RX is busy");
		return -EBUSY;
	}

	ret = modem_shell_ensure_ppp_ready(sh);
	if (ret != 0) {
		return ret;
	}

	struct modem_shell_ops ops = shellOps;
	ops.ctx = (void *)sh;
	return modem_shell_cmd_post_core(&ops, argc, argv);
}

static int cmd_modem_dns(const struct shell *sh, size_t argc, char **argv)
{
	char resolvedAddress[INET_ADDRSTRLEN];
	int dnsStatus = 0;
	int dnsError = 0;
	int ret;

	if (argc != 2U) {
		shell_error(sh, "usage: modem dns <host>");
		return -EINVAL;
	}

	if (modemUartRxOwner != MODEM_UART_RX_OWNER_NONE) {
		shell_error(sh, "modem UART RX is busy");
		return -EBUSY;
	}

	ret = modem_shell_ensure_ppp_ready(sh);
	if (ret != 0) {
		return ret;
	}

	ret = modem_shell_dns_lookup(argv[1], resolvedAddress, sizeof(resolvedAddress),
				      &dnsStatus, &dnsError);
	if (ret != 0) {
		shell_error(sh,
			    "DNS lookup failed host=%s getaddrinfo=%d errno=%d dns_timeout_ms=%d",
			    argv[1],
			    dnsStatus,
			    dnsError,
			    CONFIG_NET_SOCKETS_DNS_TIMEOUT);
		return ret;
	}

	shell_print(sh,
		    "DNS OK host=%s addr=%s dns_timeout_ms=%d",
		    argv[1],
		    resolvedAddress,
		    CONFIG_NET_SOCKETS_DNS_TIMEOUT);
	return 0;
}

static int cmd_modem_passthrough(const struct shell *sh, size_t argc, char **argv)
{
	bool debugMode = false;

	if (argc >= 2U) {
		if (strcmp(argv[1], "--debug") == 0) {
			debugMode = true;
		} else {
			shell_error(sh, "usage: modem passthrough [--debug]");
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

	if (modemUartRxOwner != MODEM_UART_RX_OWNER_NONE) {
		shell_error(sh, "modem UART RX is busy");
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

	ret = modem_uart_rx_acquire(MODEM_UART_RX_OWNER_PASSTHROUGH);
	if (ret != 0) {
		shell_error(sh, "failed to acquire modem UART IRQ RX: %d", ret);
		return ret;
	}

	passthroughShell = sh;
	passthroughTail = 0U;
	passthroughDebugMode = debugMode;
	passthroughActive = true;
	shell_print(sh,
		    debugMode ? "Entering modem UART passthrough (debug mode). Press Ctrl-X then Ctrl-Q to exit."
		              : "Entering modem UART passthrough. Press Ctrl-X then Ctrl-Q to exit.");
	shell_set_bypass(sh, modem_passthrough_bypass_cb);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_modem,
	SHELL_CMD(status, NULL, "Print modem GPIO status", cmd_modem_status),
	SHELL_CMD(reset, NULL, "Pulse modem reset (MODEM_nRST)", cmd_modem_reset),
	// SHELL_CMD_ARG(power, NULL, "Modem power control: power <on|off|cycle>", cmd_modem_power, 2, 0),
	SHELL_CMD_ARG(at, NULL, "Send AT command: at [--debug] <command>", cmd_modem_at, 2, SHELL_OPT_ARG_RAW),
	SHELL_CMD_ARG(dns, NULL, "DNS probe: dns <host>", cmd_modem_dns, 2, 0),
	SHELL_CMD_ARG(post, NULL,
		      "HTTPS POST: post <host> <port> <path> [payload]",
		      cmd_modem_post, 4, SHELL_OPT_ARG_RAW),
	SHELL_CMD_ARG(passthrough, NULL,
		      "Raw UART passthrough to modem. Use --debug for RX trace mode; Ctrl-X then Ctrl-Q exits.",
		      cmd_modem_passthrough, 1, 1),
	SHELL_SUBCMD_SET_END /* Array terminator */
);

SHELL_CMD_REGISTER(modem, &sub_modem, "Modem control", NULL);
