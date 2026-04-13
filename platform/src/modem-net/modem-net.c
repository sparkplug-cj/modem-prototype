#include "modem-at.h"
#include "modem-board.h"
#include "modem-net.h"
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
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_connectivity_impl.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/ppp.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>

#define MODEM_NET_BOOT_DELAY_MS 10000
#define MODEM_NET_SYNC_RETRIES 3
#define MODEM_NET_AT_RESPONSE_SIZE 256
#define MODEM_NET_UART_RX_BUFFER_SIZE 512
#define MODEM_NET_UART_TX_BUFFER_SIZE 512
#define MODEM_NET_PPP_FRAME_BUFFER_SIZE 128
#define MODEM_NET_PIPE_WAIT K_SECONDS(2)
#define MODEM_NET_DIAL_WAIT K_SECONDS(20)
#define MODEM_NET_CONNECT_WAIT K_SECONDS(60)
#define MODEM_NET_ESCAPE_GUARD_MS 2000
#define MODEM_NET_DEFAULT_CONTEXT_ID 1
#define MODEM_NET_AT_IRQ_RX_RING_SIZE 512
#define MODEM_NET_AT_IRQ_RX_CHUNK_SIZE 64

#define MODEM_NET_CONN_MGR_STACK_SIZE 4096

struct modem_net_conn_mgr_ctx {
	struct k_work connectWork;
	struct k_work disconnectWork;
	struct net_if *iface;
	char apn[MODEM_NET_PPP_APN_MAX_LEN];
	char id[MODEM_NET_PPP_ID_MAX_LEN];
	char password[MODEM_NET_PPP_PASSWORD_MAX_LEN];
	bool connectRequested;
};

#define PPP_IMPL_CTX_TYPE struct modem_net_conn_mgr_ctx

MODEM_PPP_DEFINE(modemNetPpp, NULL, 98, 1500, MODEM_NET_PPP_FRAME_BUFFER_SIZE);

static void ppp_net_if_init(struct conn_mgr_conn_binding *const binding);
static int ppp_net_connect(struct conn_mgr_conn_binding *const binding);
static int ppp_net_if_disconnect(struct conn_mgr_conn_binding *const binding);
static int ppp_net_if_get_opt(struct conn_mgr_conn_binding *const binding, int optname,
			      void *optval, size_t *optlen);
static int ppp_net_if_set_opt(struct conn_mgr_conn_binding *const binding, int optname,
			      const void *optval, size_t optlen);

static void modem_net_irq_at_session_close(void *ctx);


static struct conn_mgr_conn_api ppp_conn_mgr_api = {
	.init = ppp_net_if_init,
	.connect = ppp_net_connect,
	.disconnect = ppp_net_if_disconnect,
	.get_opt = ppp_net_if_get_opt,
	.set_opt = ppp_net_if_set_opt,
};

CONN_MGR_CONN_DEFINE(PPP_IMPL, &ppp_conn_mgr_api);
CONN_MGR_BIND_CONN(ppp_net_dev_modemNetPpp, PPP_IMPL);

static const struct device *const modemUart = DEVICE_DT_GET(DT_NODELABEL(modem_uart));
static struct modem_backend_uart modemNetBackend;
static uint8_t modemNetBackendRxBuffer[MODEM_NET_UART_RX_BUFFER_SIZE];
static uint8_t modemNetBackendTxBuffer[MODEM_NET_UART_TX_BUFFER_SIZE];
static uint8_t modemNetAtRxRingBuffer[MODEM_NET_AT_IRQ_RX_RING_SIZE];
static struct ring_buf modemNetAtRxRing;
static bool modemNetAtRxIrqConfigured;
static struct modem_pipe *modemNetPipe;
static struct net_if *modemNetIface;
static struct net_mgmt_event_callback modemNetMgmtCb_l2; // PPP 
static struct net_mgmt_event_callback modemNetMgmtCb_l3; // IP 
static struct net_mgmt_event_callback modemNetMgmtCb_l4; // socket
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
static struct conn_mgr_conn_binding *modemNetConnBinding;
static struct k_work_q modemNetConnMgrWorkQ;
static K_THREAD_STACK_DEFINE(modemNetConnMgrWorkQStack, MODEM_NET_CONN_MGR_STACK_SIZE);
static bool modemNetConnMgrWorkQStarted;

static void modem_net_conn_mgr_connect_work_handler(struct k_work *work);
static void modem_net_conn_mgr_disconnect_work_handler(struct k_work *work);
static struct modem_net_ops modem_net_make_ops(const struct shell *sh);

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

static void modem_net_conn_mgr_log_noop(void *ctx, const char *fmt, ...)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fmt);
}

static void modem_net_maybe_shell_print(const struct shell *sh, const char *fmt, ...)
{
	va_list args;

	if (sh == NULL) {
		return;
	}

	va_start(args, fmt);
	shell_vfprintf(sh, SHELL_NORMAL, fmt, args);
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	va_end(args);
}

static void modem_net_maybe_shell_error(const struct shell *sh, const char *fmt, ...)
{
	va_list args;

	if (sh == NULL) {
		return;
	}

	va_start(args, fmt);
	shell_vfprintf(sh, SHELL_ERROR, fmt, args);
	shell_fprintf(sh, SHELL_ERROR, "\n");
	va_end(args);
}

static void modem_net_conn_mgr_workq_start(void)
{
	if (modemNetConnMgrWorkQStarted) {
		return;
	}

	k_work_queue_init(&modemNetConnMgrWorkQ);
	k_work_queue_start(&modemNetConnMgrWorkQ,
				   modemNetConnMgrWorkQStack,
				   K_THREAD_STACK_SIZEOF(modemNetConnMgrWorkQStack),
				   K_PRIO_PREEMPT(8), NULL);
	modemNetConnMgrWorkQStarted = true;
}

static bool modem_net_conn_mgr_profile_ready(const struct modem_net_conn_mgr_ctx *ctx)
{
	return ctx->apn[0] != '\0';
}

static int modem_net_conn_mgr_copy_string(char *dst, size_t dstSize, const void *src, size_t srcLen)
{
	if ((src == NULL) || (srcLen == 0U)) {
		return -EINVAL;
	}

	if (srcLen > dstSize) {
		return -ENOBUFS;
	}

	if (memchr(src, '\0', srcLen) == NULL) {
		return -EINVAL;
	}

	memcpy(dst, src, srcLen);
	dst[srcLen - 1U] = '\0';
	return 0;
}

static int modem_net_wait_for_network_with_timeout(int timeoutSeconds)
{
	k_timeout_t timeout = MODEM_NET_CONNECT_WAIT;
	int ret;

	if (timeoutSeconds > 0) {
		timeout = K_SECONDS(timeoutSeconds);
	}

	ret = k_sem_take(&modemNetConnectedSem, timeout);
	if (ret != 0) {
		return -ETIMEDOUT;
	}

	(void)k_sem_take(&modemNetDnsSem, K_SECONDS(10));
	return 0;
}

static int modem_net_conn_mgr_connect_locked(struct modem_net_conn_mgr_ctx *ctx, int timeoutSeconds)
{
	/* --- AT transport hard reset before reconnect --- */
	modem_net_irq_at_session_close(NULL);   // RX IRQ disable + owner release
	ring_buf_reset(&modemNetAtRxRing);      // AT RX buffer flush
	k_msleep(20);                           // IRQ settle 


	struct modem_net_ops ops = modem_net_make_ops(NULL);
	const char *failedStage = NULL;
	struct modem_net_profile prof = {
		.apn = ctx->apn,
		.id = ctx->id,
		.password = ctx->password,
	};
	int ret;

	if (modemNetSessionOpen || modemNetConnected) {
		return 0;
	}

	if (!modem_net_conn_mgr_profile_ready(ctx)) {
		return 0;
	}

	ops.print = modem_net_conn_mgr_log_noop;
	ops.error = modem_net_conn_mgr_log_noop;

	ops.clear_error();

	if (ops.owner_get() != 0) {
		ops.set_error(-EBUSY, "modem UART busy");
		return -EBUSY;
	}

	ops.set_apn(prof.apn);

	ret = ops.ensure_powered(ops.ctx);
	if (ret != 0) {
		failedStage = "ensure_powered";
		goto out_fail;
	}

	ret = ops.configure_context(ops.ctx, &prof);
	if (ret != 0) {
		failedStage = "configure_context";
		goto out_fail;
	}

	ret = ops.open_uart_session();
	if (ret != 0) {
		failedStage = "open_uart_session";
		goto out_fail;
	}

	ret = ops.dial_ppp(ops.ctx);
	if (ret != 0) {
		failedStage = "dial_ppp";
		goto out_close;
	}

	ret = ops.attach_ppp();
	if (ret != 0) {
		failedStage = "attach_ppp";
		goto out_close;
	}

	ret = modem_net_wait_for_network_with_timeout(timeoutSeconds);
	if (ret != 0) {
		failedStage = "wait_for_network";
		goto out_close;
	}

	return 0;

out_close:
	(void)ops.escape_and_hangup();
	ops.close_uart_session();
out_fail:
	ops.set_error(ret, "connect failed");
	ARG_UNUSED(failedStage);
	return ret;
}

static void modem_net_conn_mgr_connect_work_handler(struct k_work *work)
{
	struct modem_net_conn_mgr_ctx *ctx = CONTAINER_OF(work, struct modem_net_conn_mgr_ctx,
						  connectWork);
	struct conn_mgr_conn_binding *binding = modemNetConnBinding;
	int timeoutSeconds = CONN_MGR_IF_NO_TIMEOUT;
	int ret;

	if ((binding == NULL) || (binding->iface == NULL) || (binding->ctx != ctx)) {
		return;
	}

	k_mutex_lock(&modemNetLock, K_FOREVER);

	if (!ctx->connectRequested) {
		k_mutex_unlock(&modemNetLock);
		return;
	}

	if (!modem_net_conn_mgr_profile_ready(ctx)) {
		k_mutex_unlock(&modemNetLock);
		return;
	}

	timeoutSeconds = binding->timeout;
	ret = modem_net_conn_mgr_connect_locked(ctx, timeoutSeconds);
	if (ret == 0) {
		ctx->connectRequested = false;
	}

	k_mutex_unlock(&modemNetLock);

	if (ret == -ETIMEDOUT) {
		net_mgmt_event_notify(NET_EVENT_CONN_IF_TIMEOUT, binding->iface);
	} else if ((ret != 0) && (ret != -EBUSY)) {
		net_mgmt_event_notify_with_info(NET_EVENT_CONN_IF_FATAL_ERROR,
					       binding->iface, &ret, sizeof(ret));
	}
}

static void modem_net_conn_mgr_disconnect_work_handler(struct k_work *work)
{
	struct modem_net_conn_mgr_ctx *ctx = CONTAINER_OF(work, struct modem_net_conn_mgr_ctx,
						  disconnectWork);
	struct modem_net_ops ops = modem_net_make_ops(NULL);

	if ((modemNetConnBinding == NULL) || (modemNetConnBinding->ctx != ctx)) {
		return;
	}

	ops.print = modem_net_conn_mgr_log_noop;
	ops.error = modem_net_conn_mgr_log_noop;

	k_mutex_lock(&modemNetLock, K_FOREVER);
	ctx->connectRequested = false;
	(void)modem_net_cmd_disconnect_core(&ops, 0, NULL);
	k_mutex_unlock(&modemNetLock);
}

static void modem_net_uart_rx_irq_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uint8_t buffer[MODEM_NET_AT_IRQ_RX_CHUNK_SIZE];

	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev)) {
		int received = uart_fifo_read(dev, buffer, sizeof(buffer));
		if (received <= 0) {
			break;
		}

		(void)ring_buf_put(&modemNetAtRxRing, buffer, (uint32_t)received);
	}
}

static int modem_net_at_rx_prepare(void)
{
	if (!device_is_ready(modemUart)) {
		return -ENODEV;
	}

	if (!modemNetAtRxIrqConfigured) {
		ring_buf_init(&modemNetAtRxRing,
			      sizeof(modemNetAtRxRingBuffer),
			      modemNetAtRxRingBuffer);
		uart_irq_callback_user_data_set(modemUart, modem_net_uart_rx_irq_cb, NULL);
		modemNetAtRxIrqConfigured = true;
	}

	return 0;
}

static uint32_t modem_net_uart_rx_read(void *ctx, uint8_t *buffer, size_t bufferSize)
{
	ARG_UNUSED(ctx);
	return ring_buf_get(&modemNetAtRxRing, buffer, bufferSize);
}

static int modem_net_irq_at_session_open(void *ctx, char *response, size_t responseSize)
{
	ARG_UNUSED(ctx);
	int ret;

	if ((response == NULL) || (responseSize == 0U)) {
		return -EINVAL;
	}

	ret = modem_net_at_rx_prepare();
	if (ret != 0) {
		return ret;
	}

	ret = modem_uart_owner_acquire(MODEM_UART_OWNER_AT);
	if (ret != 0) {
		return ret;
	}

	ring_buf_reset(&modemNetAtRxRing);
	uart_irq_rx_enable(modemUart);
	response[0] = '\0';
	return 0;
}

static void modem_net_irq_at_session_close(void *ctx)
{
	ARG_UNUSED(ctx);

	if (modem_uart_owner_get() == MODEM_UART_OWNER_AT) {
		uart_irq_rx_disable(modemUart);
		ring_buf_reset(&modemNetAtRxRing);
		modem_uart_owner_release(MODEM_UART_OWNER_AT);
	}
}

static void modem_net_event_handler(struct net_mgmt_event_callback *cb,
				      uint64_t mgmtEvent,
				      struct net_if *iface)
{
	ARG_UNUSED(cb);
	printk("Network Event Fired: 0x%08X\n", (uint32_t)mgmtEvent);

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

    // 1. L2 (PPP) 
    net_mgmt_init_event_callback(&modemNetMgmtCb_l2,
                                 modem_net_event_handler,
                                 NET_EVENT_PPP_PHASE_RUNNING |
                                 NET_EVENT_PPP_PHASE_DEAD |
                                 NET_EVENT_PPP_CARRIER_OFF);
    net_mgmt_add_event_callback(&modemNetMgmtCb_l2);

    // 2. L3 (IPv4 / DNS)
    net_mgmt_init_event_callback(&modemNetMgmtCb_l3,
                                 modem_net_event_handler,
                                 NET_EVENT_IPV4_ADDR_ADD |
                                 NET_EVENT_DNS_SERVER_ADD);
    net_mgmt_add_event_callback(&modemNetMgmtCb_l3);

    // 3. L4 (Socket)
    net_mgmt_init_event_callback(&modemNetMgmtCb_l4,
                                 modem_net_event_handler,
                                 NET_EVENT_L4_CONNECTED |
                                 NET_EVENT_L4_DISCONNECTED);
    net_mgmt_add_event_callback(&modemNetMgmtCb_l4);

    modemNetInitialized = true;
}

static int modem_net_send_at(const char *command, char *response, size_t responseSize)
{
	static const struct modem_at_irq_transport transport = {
		.ctx = NULL,
		.open = modem_net_irq_at_session_open,
		.close = modem_net_irq_at_session_close,
		.read = modem_net_uart_rx_read,
	};

	return modem_at_send_irq(command, response, responseSize, &transport, NULL);
}

static void modem_net_log_at_diagnostics(const struct shell *sh, const char *label, int ret,
					      const char *response)
{
	struct modem_at_diagnostics diagnostics = {0};

	if (sh == NULL) {
		return;
	}

	modem_at_get_last_diagnostics(&diagnostics);
	shell_error(sh,
		    "%s failed: %d (saw_bytes=%s bytes=%u exit=%s last_uart_ret=%d response='%s')",
		    label,
		    ret,
		    diagnostics.sawAnyByte ? "yes" : "no",
		    (unsigned int)diagnostics.bytesReceived,
		    modem_at_exit_reason_str(diagnostics.exitReason),
		    diagnostics.lastUartRet,
		    (response != NULL) ? response : "");
}

static int modem_net_sync_and_disable_sleep(const struct shell *sh)
{
	char response[MODEM_NET_AT_RESPONSE_SIZE];
	int ret = -ETIMEDOUT;

	modem_net_maybe_shell_print(sh, "Waiting 10s for modem boot...");
	k_msleep(MODEM_NET_BOOT_DELAY_MS);

	for (int attempt = 0; attempt < MODEM_NET_SYNC_RETRIES; ++attempt) {
		response[0] = '\0';
	modem_net_maybe_shell_print(sh, "AT sync attempt %d/%d...", attempt + 1,
				 MODEM_NET_SYNC_RETRIES);
		ret = modem_net_send_at("AT", response, sizeof(response));
		if (ret == 0) {
			modem_net_maybe_shell_print(sh, "AT sync OK: %s", response);
			break;
		}

		modem_net_log_at_diagnostics(sh, "AT sync", ret, response);
	}

	if (ret != 0) {
		return ret;
	}

	response[0] = '\0';
	modem_net_maybe_shell_print(sh, "Disabling sleep...");
	ret = modem_net_send_at("AT+KSLEEP=2", response, sizeof(response));
	if (ret != 0) {
		modem_net_log_at_diagnostics(sh, "AT+KSLEEP=2", ret, response);
		return ret;
	}

	modem_net_maybe_shell_print(sh, "Sleep disabled: %s", response);

    // disable PSM(Power Saving Mode) 
	modem_net_maybe_shell_print(sh, "Disabling PSM (CPSMS=0)...");
	ret = modem_net_send_at("AT+CPSMS=0", response, sizeof(response));
	modem_net_maybe_shell_print(sh, ":CPSMS: (%s)", (ret == 0) ? response : "FAIL");

	// disable eDRX
	modem_net_maybe_shell_print(sh, "Disabling eDRX (CEDRXS=0)...");
	ret = modem_net_send_at("AT+CEDRXS=0", response, sizeof(response));
	modem_net_maybe_shell_print(sh, ":CEDRXS: (%s)", (ret == 0) ? response : "FAIL");
	return 0;
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

	modem_net_maybe_shell_print(sh, "Powering modem...");
	ret = modem_board_power_on();
	if (ret != 0) {
		return ret;
	}

	return modem_net_sync_and_disable_sleep(sh);
}

static bool modem_cold_initialized = false;

static int modem_net_configure_context(void *ctx, const struct modem_net_profile *prof)
{
	const struct shell *sh = ctx;
	char command[96];
	char response[MODEM_NET_AT_RESPONSE_SIZE];
	int ret;

	if ((prof == NULL) || (prof->apn == NULL) || (prof->apn[0] == '\0')) {
		modem_net_maybe_shell_error(sh, "APN is not configured");
		return -EINVAL;
	}

	if(!modem_cold_initialized)
	{
		modem_net_maybe_shell_print(sh, "Resetting network stack (CFUN=4)...");
		ret = modem_net_send_at("AT+CFUN=4", response, sizeof(response));	
		modem_net_maybe_shell_print(sh, ":CFUN=4: (%s)", (ret == 0) ? response : "FAIL");

		modem_net_maybe_shell_print(sh, "Pre-activating network (CFUN=1)...");
		ret = modem_net_send_at("AT+CFUN=1", response, sizeof(response));
		modem_net_maybe_shell_print(sh, ":CFUN=1: (%s)", (ret == 0) ? response : "FAIL");

		modem_net_maybe_shell_print(sh, "Enabling verbose modem error reporting (CMEE=1)...");
		ret = modem_net_send_at("AT+CMEE=1", response, sizeof(response));
		modem_net_maybe_shell_print(sh, ":CMEE=0: (%s)", (ret == 0) ? response : "FAIL");

		snprintk(command, sizeof(command), "AT+CGDCONT=%d,\"IP\",\"%s\"",
				MODEM_NET_DEFAULT_CONTEXT_ID, prof->apn);
		modem_net_maybe_shell_print(sh, "Configuring PDP context (%s)...", prof->apn);
		ret = modem_net_send_at(command, response, sizeof(response));
		if (ret != 0) {
			return ret;
		}

		// ID/Password = a/b
		snprintk(command, sizeof(command), "AT+KCNXCFG=%d,\"GPRS\",\"%s\",\"%s\",\"%s\",\"IPV4\"",
				MODEM_NET_DEFAULT_CONTEXT_ID, prof->apn, prof->id, prof->password);
		
		modem_net_maybe_shell_print(sh, "Configuring Connection Profile (Reference Style)...");
		ret = modem_net_send_at(command, response, sizeof(response));
		if (ret != 0) {
			return ret;
		}

		modem_cold_initialized = true;
	}
	
	ret = modem_net_send_at("AT+WPPP=0", response, sizeof(response));
	modem_net_maybe_shell_print(sh, ":WPPP=0: (%s)", (ret == 0) ? response : "FAIL");

    // Waiting for registration
	modem_net_maybe_shell_print(sh, "Waiting for network registration (CREG/CEREG)...");
	int registration_attempts = 10; // max 10secs
    while (registration_attempts-- > 0) {
        ret = modem_net_send_at("AT+CEREG?", response, sizeof(response));
        
        // check response of  "+CEREG: 0,1" (home) or "+CEREG: 0,5" (roaming)
        if (ret == 0 && (strstr(response, "0,1") || strstr(response, "0,5"))) {
			modem_net_maybe_shell_print(sh, "Network registered: %s", response);
            break;
        }
        
		modem_net_maybe_shell_print(sh, "Still searching... (%d)", registration_attempts);
        k_msleep(1000);
        
        if (registration_attempts == 0) {
			modem_net_maybe_shell_error(sh, "Failed to register on network within timeout");
            return -ETIMEDOUT;
        }
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

	modem_net_maybe_shell_print(sh, "Dialing PPP...(%s)", dialCommand);
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

	modem_net_maybe_shell_print(sh, "PPP data mode entered");
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
	return 0;// it's done in conn_mgr_if_connect()     net_if_up(modemNetIface);
}

struct net_if *modem_net_ppp_iface_get(void)
{
	return modem_ppp_get_iface(&modemNetPpp);
}

static int modem_net_wait_for_network(void *ctx)
{
	const struct shell *sh = ctx;
	int ret;

	modem_net_maybe_shell_print(sh, "Waiting for IP link...");
	ret = k_sem_take(&modemNetConnectedSem, MODEM_NET_CONNECT_WAIT);
	if (ret != 0) {
		return -ETIMEDOUT;
	}

	modem_net_maybe_shell_print(sh, "Waiting for DNS...");
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

	if ((modemNetPipe == NULL) || !modemNetSessionOpen) {
		return 0;
	}

	k_msleep(MODEM_NET_ESCAPE_GUARD_MS);
	(void)modem_pipe_transmit(modemNetPipe, escapeSequence, sizeof(escapeSequence) - 1U);
	k_msleep(MODEM_NET_ESCAPE_GUARD_MS);
	(void)modem_pipe_transmit(modemNetPipe, hangupCommand, sizeof(hangupCommand) - 1U);
	k_msleep(500);
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

static void ppp_net_if_init(struct conn_mgr_conn_binding *const binding)
{
	struct modem_net_conn_mgr_ctx *ctx = binding->ctx;

	modem_net_init_once();
	modem_net_conn_mgr_workq_start();
	k_work_init(&ctx->connectWork, modem_net_conn_mgr_connect_work_handler);
	k_work_init(&ctx->disconnectWork, modem_net_conn_mgr_disconnect_work_handler);
	ctx->iface = binding->iface;
	modemNetConnBinding = binding;
	modemNetIface = binding->iface;
	conn_mgr_binding_set_flag(binding, CONN_MGR_IF_NO_AUTO_CONNECT, true);
	conn_mgr_binding_set_flag(binding, CONN_MGR_IF_NO_AUTO_DOWN, true);
}

static int ppp_net_connect(struct conn_mgr_conn_binding *const binding)
{
	struct modem_net_conn_mgr_ctx *ctx = binding->ctx;
	int ret;

	ctx->connectRequested = true;
	ret = k_work_submit_to_queue(&modemNetConnMgrWorkQ, &ctx->connectWork);
	return (ret < 0) ? ret : 0;
}

static int ppp_net_if_disconnect(struct conn_mgr_conn_binding *const binding)
{
	struct modem_net_conn_mgr_ctx *ctx = binding->ctx;
	int ret;

	ctx->connectRequested = false;
	ret = k_work_submit_to_queue(&modemNetConnMgrWorkQ, &ctx->disconnectWork);
	return (ret < 0) ? ret : 0;
}

static int ppp_net_if_get_opt(struct conn_mgr_conn_binding *const binding, int optname,
			      void *optval, size_t *optlen)
{
	struct modem_net_conn_mgr_ctx *ctx = binding->ctx;
	struct modem_net_ppp_profile *profile;
	const char *src = NULL;
	size_t needed = 0U;

	if ((optval == NULL) || (optlen == NULL)) {
		return -EINVAL;
	}

	switch (optname) {
	case MODEM_NET_PPP_OPT_APN:
		src = ctx->apn;
		break;
	case MODEM_NET_PPP_OPT_ID:
		src = ctx->id;
		break;
	case MODEM_NET_PPP_OPT_PASSWORD:
		src = ctx->password;
		break;
	case MODEM_NET_PPP_OPT_PROFILE:
		needed = sizeof(*profile);
		if (*optlen < needed) {
			*optlen = needed;
			return -ENOBUFS;
		}
		profile = optval;
		memset(profile, 0, sizeof(*profile));
		memcpy(profile->apn, ctx->apn, sizeof(profile->apn));
		memcpy(profile->id, ctx->id, sizeof(profile->id));
		memcpy(profile->password, ctx->password, sizeof(profile->password));
		*optlen = needed;
		return 0;
	default:
		*optlen = 0U;
		return -ENOPROTOOPT;
	}

	needed = strlen(src) + 1U;
	if (*optlen < needed) {
		*optlen = needed;
		return -ENOBUFS;
	}

	memcpy(optval, src, needed);
	*optlen = needed;
	return 0;
}

static int ppp_net_if_set_opt(struct conn_mgr_conn_binding *const binding, int optname,
			      const void *optval, size_t optlen)
{
	struct modem_net_conn_mgr_ctx *ctx = binding->ctx;
	const struct modem_net_ppp_profile *profile;
	int ret;

	switch (optname) {
	case MODEM_NET_PPP_OPT_APN:
		ret = modem_net_conn_mgr_copy_string(ctx->apn, sizeof(ctx->apn), optval, optlen);
		break;
	case MODEM_NET_PPP_OPT_ID:
		ret = modem_net_conn_mgr_copy_string(ctx->id, sizeof(ctx->id), optval, optlen);
		break;
	case MODEM_NET_PPP_OPT_PASSWORD:
		ret = modem_net_conn_mgr_copy_string(ctx->password, sizeof(ctx->password), optval,
					    optlen);
		break;
	case MODEM_NET_PPP_OPT_PROFILE:
		if ((optval == NULL) || (optlen != sizeof(*profile))) {
			return -EINVAL;
		}
		profile = optval;
		memcpy(ctx->apn, profile->apn, sizeof(ctx->apn));
		ctx->apn[sizeof(ctx->apn) - 1U] = '\0';
		memcpy(ctx->id, profile->id, sizeof(ctx->id));
		ctx->id[sizeof(ctx->id) - 1U] = '\0';
		memcpy(ctx->password, profile->password, sizeof(ctx->password));
		ctx->password[sizeof(ctx->password) - 1U] = '\0';
		ret = 0;
		break;
	default:
		return -ENOPROTOOPT;
	}

	if ((ret == 0) && ctx->connectRequested && modem_net_conn_mgr_profile_ready(ctx)) {
		ret = k_work_submit_to_queue(&modemNetConnMgrWorkQ, &ctx->connectWork);
		if (ret >= 0) {
			ret = 0;
		}
	}

	return ret;
}

int cmd_modem_ppp_connect(const struct shell *sh, size_t argc, char **argv)
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

int cmd_modem_ppp_disconnect(const struct shell *sh, size_t argc, char **argv)
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

int cmd_modem_ppp_status(const struct shell *sh, size_t argc, char **argv)
{
	int ret;
	struct modem_net_ops ops;

	modem_net_init_once();
	ops = modem_net_make_ops(sh);
	ret = modem_net_cmd_status_core(&ops, argc, argv);
	return ret;
}

