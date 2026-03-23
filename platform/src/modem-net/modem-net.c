#include "modem-at.h"
#include "modem-board.h"
#include "modem-net-core.h"
#include "modem-shell-uart.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/modem/ppp.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/ppp.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>

#define MODEM_NET_BOOT_DELAY_MS 10000
#define MODEM_NET_SYNC_RETRIES 3
#define MODEM_NET_AT_RESPONSE_SIZE 256
#define MODEM_NET_UART_RX_BUFFER_SIZE 512
#define MODEM_NET_UART_TX_BUFFER_SIZE 512
#define MODEM_NET_PPP_FRAME_BUFFER_SIZE 128
#define MODEM_NET_PIPE_WAIT K_SECONDS(2)
#define MODEM_NET_DIAL_WAIT K_SECONDS(20)
#define MODEM_NET_CONNECT_WAIT K_SECONDS(60)
#define MODEM_NET_ESCAPE_GUARD_MS 1200
#define MODEM_NET_DEFAULT_CONTEXT_ID 1

MODEM_PPP_DEFINE(modemNetPpp, NULL, 98, 1500, MODEM_NET_PPP_FRAME_BUFFER_SIZE);

static const struct device *const modemUart = DEVICE_DT_GET(DT_NODELABEL(modem_uart));
static struct modem_backend_uart modemNetBackend;
static uint8_t modemNetBackendRxBuffer[MODEM_NET_UART_RX_BUFFER_SIZE];
static uint8_t modemNetBackendTxBuffer[MODEM_NET_UART_TX_BUFFER_SIZE];
static struct modem_pipe *modemNetPipe;
static struct net_if *modemNetIface;
static struct net_mgmt_event_callback modemNetMgmtCb;
static struct k_sem modemNetConnectedSem;
static struct k_sem modemNetDnsSem;
static struct k_mutex modemNetLock;
static bool modemNetInitialized;
static bool modemNetSessionOpen;
static bool modemNetPppAttached;
static bool modemNetConnected;
static bool modemNetDnsReady;
static int modemNetLastError;
static char modemNetLastErrorText[96];
static char modemNetApn[64];

static void modem_net_set_error(int error, const char *message)
{
	modemNetLastError = error;
	snprintk(modemNetLastErrorText, sizeof(modemNetLastErrorText), "%s", message);
}

static void modem_net_clear_error(void)
{
	modemNetLastError = 0;
	modemNetLastErrorText[0] = '\0';
}

static void modem_net_event_handler(struct net_mgmt_event_callback *cb,
				      uint64_t mgmtEvent,
				      struct net_if *iface)
{
	ARG_UNUSED(cb);

	if ((modemNetIface != NULL) && (iface != modemNetIface)) {
		return;
	}

	if ((mgmtEvent == NET_EVENT_L4_CONNECTED) ||
	    (mgmtEvent == NET_EVENT_IPV4_ADDR_ADD) ||
	    (mgmtEvent == NET_EVENT_PPP_PHASE_RUNNING)) {
		modemNetConnected = true;
		k_sem_give(&modemNetConnectedSem);
	}

	if (mgmtEvent == NET_EVENT_DNS_SERVER_ADD) {
		modemNetDnsReady = true;
		k_sem_give(&modemNetDnsSem);
	}

	if ((mgmtEvent == NET_EVENT_L4_DISCONNECTED) ||
	    (mgmtEvent == NET_EVENT_PPP_PHASE_DEAD) ||
	    (mgmtEvent == NET_EVENT_PPP_CARRIER_OFF)) {
		modemNetConnected = false;
	}
}

static void modem_net_init_once(void)
{
	if (modemNetInitialized) {
		return;
	}

	k_sem_init(&modemNetConnectedSem, 0, 1);
	k_sem_init(&modemNetDnsSem, 0, 1);
	k_mutex_init(&modemNetLock);
	net_mgmt_init_event_callback(&modemNetMgmtCb,
				     modem_net_event_handler,
				     NET_EVENT_L4_CONNECTED |
				     NET_EVENT_L4_DISCONNECTED |
				     NET_EVENT_IPV4_ADDR_ADD |
				     NET_EVENT_DNS_SERVER_ADD |
				     NET_EVENT_PPP_PHASE_RUNNING |
				     NET_EVENT_PPP_PHASE_DEAD |
				     NET_EVENT_PPP_CARRIER_OFF);
	net_mgmt_add_event_callback(&modemNetMgmtCb);
	modemNetInitialized = true;
}

static int modem_net_send_at(const char *command, char *response, size_t responseSize)
{
	return modem_at_send(command, response, responseSize);
}

static int modem_net_sync_and_disable_sleep(const struct shell *sh)
{
	char response[MODEM_NET_AT_RESPONSE_SIZE];
	int ret = -ETIMEDOUT;

	shell_print(sh, "Waiting 10s for modem boot...");
	k_msleep(MODEM_NET_BOOT_DELAY_MS);

	for (int attempt = 0; attempt < MODEM_NET_SYNC_RETRIES; ++attempt) {
		ret = modem_net_send_at("AT", response, sizeof(response));
		if (ret == 0) {
			break;
		}
	}

	if (ret != 0) {
		return ret;
	}

	shell_print(sh, "Disabling sleep...");
	return modem_net_send_at("AT+KSLEEP=2", response, sizeof(response));
}

static int modem_net_ensure_powered(void *ctx)
{
	const struct shell *sh = ctx;
	struct modem_board_status status;
	int ret = modem_board_get_status(&status);
	if (ret != 0) {
		return ret;
	}

	if (status.rail_en == 1) {
		return 0;
	}

	shell_print(sh, "Powering modem...");
	ret = modem_board_power_on();
	if (ret != 0) {
		return ret;
	}

	return modem_net_sync_and_disable_sleep(sh);
}

static int modem_net_configure_context(void *ctx, const char *apn)
{
	const struct shell *sh = ctx;
	char command[96];
	char response[MODEM_NET_AT_RESPONSE_SIZE];
	int ret;

	if ((apn == NULL) || (apn[0] == '\0')) {
		shell_error(sh, "APN is not configured");
		return -EINVAL;
	}

	snprintk(command, sizeof(command), "AT+CGDCONT=%d,\"IP\",\"%s\"",
		 MODEM_NET_DEFAULT_CONTEXT_ID,
		 apn);
	shell_print(sh, "Configuring PDP context (%s)...", apn);
	ret = modem_net_send_at(command, response, sizeof(response));
	if (ret != 0) {
		return ret;
	}

	ret = modem_net_send_at("AT+CGATT=1", response, sizeof(response));
	if ((ret != 0) && (ret != -EIO)) {
		return ret;
	}

	ret = modem_net_send_at("AT+CGACT=1,1", response, sizeof(response));
	if ((ret != 0) && (ret != -EIO)) {
		return ret;
	}

	return 0;
}

static int modem_net_open_uart_session(void)
{
	const struct modem_backend_uart_config config = {
		.uart = modemUart,
		.receive_buf = modemNetBackendRxBuffer,
		.receive_buf_size = sizeof(modemNetBackendRxBuffer),
		.transmit_buf = modemNetBackendTxBuffer,
		.transmit_buf_size = sizeof(modemNetBackendTxBuffer),
	};
	int ret;

	if (!device_is_ready(modemUart)) {
		return -ENODEV;
	}

	ret = modem_uart_owner_acquire(MODEM_UART_OWNER_PPP);
	if (ret != 0) {
		return ret;
	}

	modemNetPipe = modem_backend_uart_init(&modemNetBackend, &config);
	if (modemNetPipe == NULL) {
		modem_uart_owner_release(MODEM_UART_OWNER_PPP);
		return -ENODEV;
	}

	ret = modem_pipe_open(modemNetPipe, MODEM_NET_PIPE_WAIT);
	if (ret != 0) {
		modem_uart_owner_release(MODEM_UART_OWNER_PPP);
		modemNetPipe = NULL;
		return ret;
	}

	modemNetSessionOpen = true;
	return 0;
}

static int modem_net_wait_for_connect_text(char *buffer, size_t bufferSize)
{
	int64_t deadline = k_uptime_get() + k_ticks_to_ms_floor64(MODEM_NET_DIAL_WAIT.ticks);
	size_t used = 0U;

	while (k_uptime_get() < deadline) {
		int ret = modem_pipe_receive(modemNetPipe,
					    (uint8_t *)&buffer[used],
					    bufferSize - used - 1U);
		if (ret > 0) {
			used += (size_t)ret;
			buffer[used] = '\0';
			if (strstr(buffer, "CONNECT") != NULL) {
				return 0;
			}
			if (strstr(buffer, "NO CARRIER") != NULL) {
				return -ENOTCONN;
			}
			if (strstr(buffer, "ERROR") != NULL) {
				return -EIO;
			}
		}

		k_msleep(20);
	}

	return -ETIMEDOUT;
}

static int modem_net_dial_ppp(void *ctx)
{
	const struct shell *sh = ctx;
	static const char dialCommand[] = "ATD*99***1#\r";
	char response[MODEM_NET_AT_RESPONSE_SIZE] = {0};
	int ret;

	shell_print(sh, "Dialing PPP...");
	ret = modem_pipe_transmit(modemNetPipe,
				 (const uint8_t *)dialCommand,
				 sizeof(dialCommand) - 1U);
	if (ret < 0) {
		return ret;
	}

	ret = modem_net_wait_for_connect_text(response, sizeof(response));
	if (ret != 0) {
		return ret;
	}

	shell_print(sh, "PPP data mode entered");
	return 0;
}

static int modem_net_attach_ppp(void)
{
	int ret = modem_ppp_attach(&modemNetPpp, modemNetPipe);
	if (ret != 0) {
		return ret;
	}

	modemNetPppAttached = true;
	modemNetIface = modem_ppp_get_iface(&modemNetPpp);
	if (modemNetIface == NULL) {
		return -ENODEV;
	}

	net_if_carrier_on(modemNetIface);
	return net_if_up(modemNetIface);
}

static int modem_net_wait_for_network(void *ctx)
{
	const struct shell *sh = ctx;
	int ret;

	shell_print(sh, "Waiting for IP link...");
	ret = k_sem_take(&modemNetConnectedSem, MODEM_NET_CONNECT_WAIT);
	if (ret != 0) {
		return -ETIMEDOUT;
	}

	shell_print(sh, "Waiting for DNS...");
	(void)k_sem_take(&modemNetDnsSem, K_SECONDS(10));
	return 0;
}

static void modem_net_close_uart_session(void)
{
	if (modemNetPppAttached) {
		modem_ppp_release(&modemNetPpp);
		modemNetPppAttached = false;
	}

	if (modemNetIface != NULL) {
		net_if_carrier_off(modemNetIface);
		(void)net_if_down(modemNetIface);
	}

	if (modemNetSessionOpen && (modemNetPipe != NULL)) {
		(void)modem_pipe_close(modemNetPipe, MODEM_NET_PIPE_WAIT);
	}

	modemNetPipe = NULL;
	modemNetIface = NULL;
	modemNetSessionOpen = false;
	modemNetConnected = false;
	modemNetDnsReady = false;
	k_sem_reset(&modemNetConnectedSem);
	k_sem_reset(&modemNetDnsSem);
	modem_uart_owner_release(MODEM_UART_OWNER_PPP);
}

static int modem_net_escape_and_hangup(void)
{
	static const uint8_t escapeSequence[] = "+++";
	static const uint8_t hangupCommand[] = "ATH\r";
	uint8_t rxBuffer[64];

	k_msleep(MODEM_NET_ESCAPE_GUARD_MS);
	(void)modem_pipe_transmit(modemNetPipe, escapeSequence, sizeof(escapeSequence) - 1U);
	k_msleep(MODEM_NET_ESCAPE_GUARD_MS);
	(void)modem_pipe_receive(modemNetPipe, rxBuffer, sizeof(rxBuffer));
	(void)modem_pipe_transmit(modemNetPipe, hangupCommand, sizeof(hangupCommand) - 1U);
	k_msleep(500);
	(void)modem_pipe_receive(modemNetPipe, rxBuffer, sizeof(rxBuffer));
	return 0;
}

static void modem_net_shell_print(void *ctx, const char *fmt, ...)
{
	const struct shell *sh = ctx;
	va_list args;

	va_start(args, fmt);
	shell_vfprintf(sh, SHELL_NORMAL, fmt, args);
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	va_end(args);
}

static void modem_net_shell_error(void *ctx, const char *fmt, ...)
{
	const struct shell *sh = ctx;
	va_list args;

	va_start(args, fmt);
	shell_vfprintf(sh, SHELL_ERROR, fmt, args);
	shell_fprintf(sh, SHELL_ERROR, "\n");
	va_end(args);
}

static void modem_net_set_apn(const char *apn)
{
	snprintk(modemNetApn, sizeof(modemNetApn), "%s", apn);
}

static int modem_net_owner_get(void)
{
	return (int)modem_uart_owner_get();
}

static int modem_net_get_status(struct modem_net_status *out)
{
	struct modem_board_status boardStatus;
	char *ipv4 = NULL;
	int ret;

	if (out == NULL) {
		return -EINVAL;
	}

	ret = modem_board_get_status(&boardStatus);
	if (ret != 0) {
		return ret;
	}

	memset(out, 0, sizeof(*out));
	out->modemPowered = boardStatus.modem_state_on;
	out->sessionOpen = modemNetSessionOpen;
	out->connected = modemNetConnected;
	out->dnsReady = modemNetDnsReady;
	out->uartOwner = modem_uart_owner_get();
	out->lastError = modemNetLastError;
	out->lastErrorText = modemNetLastErrorText;
	out->apn = (modemNetApn[0] != '\0') ? modemNetApn : NULL;

	if (modemNetIface != NULL) {
		struct in_addr *addr = net_if_ipv4_get_global_addr(modemNetIface, NET_ADDR_PREFERRED);
		static char ipv4Buffer[NET_IPV4_ADDR_LEN];
		if (addr != NULL) {
			ipv4 = net_addr_ntop(AF_INET, addr, ipv4Buffer, sizeof(ipv4Buffer));
		}
	}

	out->ipv4 = ipv4;
	return 0;
}

static struct modem_net_ops modem_net_make_ops(const struct shell *sh)
{
	return (struct modem_net_ops){
		.owner_get = modem_net_owner_get,
		.ensure_powered = modem_net_ensure_powered,
		.configure_context = modem_net_configure_context,
		.open_uart_session = modem_net_open_uart_session,
		.dial_ppp = modem_net_dial_ppp,
		.attach_ppp = modem_net_attach_ppp,
		.wait_for_network = modem_net_wait_for_network,
		.close_uart_session = modem_net_close_uart_session,
		.escape_and_hangup = modem_net_escape_and_hangup,
		.get_status = modem_net_get_status,
		.set_apn = modem_net_set_apn,
		.set_error = modem_net_set_error,
		.clear_error = modem_net_clear_error,
		.print = modem_net_shell_print,
		.error = modem_net_shell_error,
		.ctx = (void *)sh,
	};
}

static int cmd_modem_ppp_connect(const struct shell *sh, size_t argc, char **argv)
{
	int ret;
	struct modem_net_ops ops;

	modem_net_init_once();
	k_mutex_lock(&modemNetLock, K_FOREVER);
	ops = modem_net_make_ops(sh);
	ret = modem_net_cmd_connect_core(&ops, argc, argv);
	k_mutex_unlock(&modemNetLock);
	return ret;
}

static int cmd_modem_ppp_disconnect(const struct shell *sh, size_t argc, char **argv)
{
	int ret;
	struct modem_net_ops ops;

	modem_net_init_once();
	k_mutex_lock(&modemNetLock, K_FOREVER);
	ops = modem_net_make_ops(sh);
	ret = modem_net_cmd_disconnect_core(&ops, argc, argv);
	k_mutex_unlock(&modemNetLock);
	return ret;
}

static int cmd_modem_ppp_status(const struct shell *sh, size_t argc, char **argv)
{
	int ret;
	struct modem_net_ops ops;

	modem_net_init_once();
	ops = modem_net_make_ops(sh);
	ret = modem_net_cmd_status_core(&ops, argc, argv);
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_modem_ppp,
	SHELL_CMD_ARG(connect, NULL, "Bring up modem PPP link: ppp connect <apn>", cmd_modem_ppp_connect, 1, 1),
	SHELL_CMD_ARG(disconnect, NULL, "Tear down modem PPP link", cmd_modem_ppp_disconnect, 1, 0),
	SHELL_CMD_ARG(status, NULL, "Show modem PPP status", cmd_modem_ppp_status, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_SUBCMD_ADD((modem), ppp, &sub_modem_ppp,
		 "Modem PPP control.",
		 cmd_modem_ppp_status, 1, 0);
