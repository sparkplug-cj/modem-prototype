#define DT_DRV_COMPAT fph_rc7620

#include "modem-rc7620.h"
#include <modem-link-conn-mgr.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/cellular.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/modem/chat.h>
#include <zephyr/modem/cmux.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/modem/ppp.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/ppp.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util_macro.h>

LOG_MODULE_REGISTER(modem_rc7620, LOG_LEVEL_INF);

#undef MODEM_CHAT_MATCH_DEFINE
#define MODEM_CHAT_MATCH_DEFINE(_sym, _match, _separators, _callback)                             \
	static const struct modem_chat_match _sym = MODEM_CHAT_MATCH(_match, _separators,          \
								     _callback)

#undef MODEM_CHAT_MATCHES_DEFINE
#define MODEM_CHAT_MATCHES_DEFINE(_sym, ...)                                                      \
	static const struct modem_chat_match _sym[] = { __VA_ARGS__ }

#undef MODEM_CHAT_SCRIPT_CMDS_DEFINE
#define MODEM_CHAT_SCRIPT_CMDS_DEFINE(_sym, ...)                                                  \
	static const struct modem_chat_script_chat _sym[] = { __VA_ARGS__ }

#undef MODEM_CHAT_SCRIPT_DEFINE
#define MODEM_CHAT_SCRIPT_DEFINE(_sym, _script_chats, _abort_matches, _callback, _timeout_s)     \
	static const struct modem_chat_script _sym = {                                             \
		.name = #_sym,                                                                     \
		.script_chats = _script_chats,                                                     \
		.script_chats_size = ARRAY_SIZE(_script_chats),                                    \
		.abort_matches = _abort_matches,                                                   \
		.abort_matches_size = ARRAY_SIZE(_abort_matches),                                  \
		.callback = _callback,                                                             \
		.timeout = _timeout_s,                                                             \
	}

#define MODEM_RC7620_INFO_IMEI_LEN         16
#define MODEM_RC7620_INFO_MODEL_LEN        65
#define MODEM_RC7620_INFO_IMSI_LEN         23
#define MODEM_RC7620_INFO_ICCID_LEN        22
#define MODEM_RC7620_INFO_MANUFACTURER_LEN 65
#define MODEM_RC7620_INFO_FW_LEN           65

#define MODEM_RC7620_PROFILE_VALUE_LEN     64
#define MODEM_RC7620_CGDCONT_COMMAND_LEN   96
#define MODEM_RC7620_KCNXCFG_COMMAND_LEN   256

#define MODEM_RC7620_PERIODIC_TIMEOUT K_MSEC(CONFIG_MODEM_RC7620_PERIODIC_SCRIPT_MS)
#define MODEM_RC7620_CMUX_DELAY_MS   100
#define MODEM_RC7620_DIAL_DELAY_MS   100
#define MODEM_RC7620_DIAL_CONNECT_TIMEOUT K_SECONDS(20)
#define MODEM_RC7620_RAIL_SETTLE_MS  10
#define MODEM_RC7620_EVENT_BUFFER_SIZE 32
/* Zephyr CMUX enforces a hard minimum DLCI RX ring size independent of MTU. */
#define MODEM_RC7620_DLCI_BUFFER_SIZE MAX(CONFIG_MODEM_CMUX_WORK_BUFFER_SIZE, 126)

#define CSQ_RSSI_UNKNOWN 99
#define CESQ_RSRP_UNKNOWN 255
#define CESQ_RSRQ_UNKNOWN 255

#define CSQ_RSSI_TO_DB(value) (-113 + (2 * (value)))
#define CESQ_RSRP_TO_DB(value) (-140 + (value))
#define CESQ_RSRQ_TO_DB(value) (-20 + ((value) / 2))

enum modem_rc7620_state {
	MODEM_RC7620_STATE_IDLE = 0,
	MODEM_RC7620_STATE_RESET_PULSE,
	MODEM_RC7620_STATE_POWER_ON_PULSE,
	MODEM_RC7620_STATE_AWAIT_POWER_ON,
	MODEM_RC7620_STATE_RUN_INIT_SCRIPT,
	MODEM_RC7620_STATE_CONNECT_CMUX,
	MODEM_RC7620_STATE_OPEN_DLCI1,
	MODEM_RC7620_STATE_OPEN_DLCI2,
	MODEM_RC7620_STATE_RUN_DIAL_SCRIPT,
	MODEM_RC7620_STATE_AWAIT_REGISTERED,
	MODEM_RC7620_STATE_CARRIER_ON,
	MODEM_RC7620_STATE_DORMANT,
	MODEM_RC7620_STATE_INIT_POWER_OFF,
	MODEM_RC7620_STATE_RUN_SHUTDOWN_SCRIPT,
	MODEM_RC7620_STATE_POWER_OFF_PULSE,
	MODEM_RC7620_STATE_AWAIT_POWER_OFF,
};

enum modem_rc7620_event {
	MODEM_RC7620_EVENT_RESUME = 0,
	MODEM_RC7620_EVENT_SUSPEND,
	MODEM_RC7620_EVENT_SCRIPT_SUCCESS,
	MODEM_RC7620_EVENT_SCRIPT_FAILED,
	MODEM_RC7620_EVENT_CMUX_CONNECTED,
	MODEM_RC7620_EVENT_DLCI1_OPENED,
	MODEM_RC7620_EVENT_DLCI2_OPENED,
	MODEM_RC7620_EVENT_TIMEOUT,
	MODEM_RC7620_EVENT_REGISTERED,
	MODEM_RC7620_EVENT_DEREGISTERED,
	MODEM_RC7620_EVENT_BUS_OPENED,
	MODEM_RC7620_EVENT_BUS_RX_READY,
	MODEM_RC7620_EVENT_BUS_CLOSED,
	MODEM_RC7620_EVENT_PPP_DEAD,
};

static const char *modem_rc7620_state_name(enum modem_rc7620_state state)
{
	switch (state) {
	case MODEM_RC7620_STATE_IDLE:
		return "IDLE";
	case MODEM_RC7620_STATE_RESET_PULSE:
		return "RESET_PULSE";
	case MODEM_RC7620_STATE_POWER_ON_PULSE:
		return "POWER_ON_PULSE";
	case MODEM_RC7620_STATE_AWAIT_POWER_ON:
		return "AWAIT_POWER_ON";
	case MODEM_RC7620_STATE_RUN_INIT_SCRIPT:
		return "RUN_INIT_SCRIPT";
	case MODEM_RC7620_STATE_CONNECT_CMUX:
		return "CONNECT_CMUX";
	case MODEM_RC7620_STATE_OPEN_DLCI1:
		return "OPEN_DLCI1";
	case MODEM_RC7620_STATE_OPEN_DLCI2:
		return "OPEN_DLCI2";
	case MODEM_RC7620_STATE_RUN_DIAL_SCRIPT:
		return "RUN_DIAL_SCRIPT";
	case MODEM_RC7620_STATE_AWAIT_REGISTERED:
		return "AWAIT_REGISTERED";
	case MODEM_RC7620_STATE_CARRIER_ON:
		return "CARRIER_ON";
	case MODEM_RC7620_STATE_DORMANT:
		return "DORMANT";
	case MODEM_RC7620_STATE_INIT_POWER_OFF:
		return "INIT_POWER_OFF";
	case MODEM_RC7620_STATE_RUN_SHUTDOWN_SCRIPT:
		return "RUN_SHUTDOWN_SCRIPT";
	case MODEM_RC7620_STATE_POWER_OFF_PULSE:
		return "POWER_OFF_PULSE";
	case MODEM_RC7620_STATE_AWAIT_POWER_OFF:
		return "AWAIT_POWER_OFF";
	default:
		return "UNKNOWN";
	}
}

static const char *modem_rc7620_event_name(enum modem_rc7620_event event)
{
	switch (event) {
	case MODEM_RC7620_EVENT_RESUME:
		return "RESUME";
	case MODEM_RC7620_EVENT_SUSPEND:
		return "SUSPEND";
	case MODEM_RC7620_EVENT_SCRIPT_SUCCESS:
		return "SCRIPT_SUCCESS";
	case MODEM_RC7620_EVENT_SCRIPT_FAILED:
		return "SCRIPT_FAILED";
	case MODEM_RC7620_EVENT_CMUX_CONNECTED:
		return "CMUX_CONNECTED";
	case MODEM_RC7620_EVENT_DLCI1_OPENED:
		return "DLCI1_OPENED";
	case MODEM_RC7620_EVENT_DLCI2_OPENED:
		return "DLCI2_OPENED";
	case MODEM_RC7620_EVENT_TIMEOUT:
		return "TIMEOUT";
	case MODEM_RC7620_EVENT_REGISTERED:
		return "REGISTERED";
	case MODEM_RC7620_EVENT_DEREGISTERED:
		return "DEREGISTERED";
	case MODEM_RC7620_EVENT_BUS_OPENED:
		return "BUS_OPENED";
	case MODEM_RC7620_EVENT_BUS_RX_READY:
		return "BUS_RX_READY";
	case MODEM_RC7620_EVENT_BUS_CLOSED:
		return "BUS_CLOSED";
	case MODEM_RC7620_EVENT_PPP_DEAD:
		return "PPP_DEAD";
	default:
		return "UNKNOWN";
	}
}

static const char *modem_rc7620_pipe_event_name(enum modem_pipe_event event)
{
	switch (event) {
	case MODEM_PIPE_EVENT_OPENED:
		return "OPENED";
	case MODEM_PIPE_EVENT_RECEIVE_READY:
		return "RECEIVE_READY";
	case MODEM_PIPE_EVENT_TRANSMIT_IDLE:
		return "TRANSMIT_IDLE";
	case MODEM_PIPE_EVENT_CLOSED:
		return "CLOSED";
	default:
		return "OTHER";
	}
}

static const char *modem_rc7620_script_result_name(enum modem_chat_script_result result)
{
	switch (result) {
	case MODEM_CHAT_SCRIPT_RESULT_SUCCESS:
		return "SUCCESS";
	case MODEM_CHAT_SCRIPT_RESULT_ABORT:
		return "ABORT";
	case MODEM_CHAT_SCRIPT_RESULT_TIMEOUT:
		return "TIMEOUT";
	default:
		return "UNKNOWN";
	}
}

static const char *modem_rc7620_ppp_event_name(uint64_t event)
{
	switch (event) {
	case NET_EVENT_PPP_CARRIER_ON:
		return "PPP_CARRIER_ON";
	case NET_EVENT_PPP_CARRIER_OFF:
		return "PPP_CARRIER_OFF";
	case NET_EVENT_PPP_PHASE_RUNNING:
		return "PPP_PHASE_RUNNING";
	case NET_EVENT_PPP_PHASE_DEAD:
		return "PPP_PHASE_DEAD";
	default:
		return "PPP_EVENT_UNKNOWN";
	}
}

static void modem_rc7620_log_script(const struct modem_chat_script *script)
{
	uint16_t index;

	if (script == NULL) {
		return;
	}

	for (index = 0U; index < script->script_chats_size; ++index) {
		const struct modem_chat_script_chat *chat = &script->script_chats[index];

		if ((chat->request == NULL) || (chat->request_size == 0U)) {
			LOG_INF("AT[%s:%u] > <empty>", script->name, index);
			continue;
		}

		LOG_INF("AT[%s:%u] > %.*s", script->name, index,
			(int)chat->request_size, (const char *)chat->request);
	}
}

static int modem_rc7620_run_script_async(enum modem_rc7620_state state,
					 struct modem_chat *chat,
					 const struct modem_chat_script *script)
{
	LOG_INF("Starting script %s in state %s", script->name,
		modem_rc7620_state_name(state));
	modem_rc7620_log_script(script);
	return modem_chat_run_script_async(chat, script);
}

struct modem_rc7620_data {
	struct modem_pipe *uartPipe;
	struct modem_backend_uart uartBackend;
	uint8_t uartRxBuffer[CONFIG_MODEM_RC7620_UART_BUFFER_SIZE];
	uint8_t uartTxBuffer[CONFIG_MODEM_RC7620_UART_BUFFER_SIZE];

	struct modem_cmux cmux;
	uint8_t cmuxRxBuffer[CONFIG_MODEM_CMUX_WORK_BUFFER_SIZE];
	uint8_t cmuxTxBuffer[CONFIG_MODEM_CMUX_WORK_BUFFER_SIZE];

	struct modem_cmux_dlci dlci1;
	struct modem_cmux_dlci dlci2;
	struct modem_pipe *dlci1Pipe;
	struct modem_pipe *dlci2Pipe;
	struct modem_pipe *cmdPipe;
	uint8_t dlci1RxBuffer[MODEM_RC7620_DLCI_BUFFER_SIZE];
	uint8_t dlci2RxBuffer[MODEM_RC7620_DLCI_BUFFER_SIZE];

	struct modem_chat chat;
	uint8_t chatRxBuffer[CONFIG_MODEM_RC7620_CHAT_BUFFER_SIZE];
	uint8_t *chatDelimiter;
	uint8_t *chatFilter;
	uint8_t *chatArgv[24];
	struct modem_chat_script_chat dialScriptCmds[5];
	struct modem_chat_script dialScript;
	char apn[MODEM_RC7620_PROFILE_VALUE_LEN];
	char id[MODEM_RC7620_PROFILE_VALUE_LEN];
	char password[MODEM_RC7620_PROFILE_VALUE_LEN];
	char cgdcontCommand[MODEM_RC7620_CGDCONT_COMMAND_LEN];
	char kcnxcfgCommand[MODEM_RC7620_KCNXCFG_COMMAND_LEN];
	struct k_mutex profileLock;

	enum cellular_registration_status registrationStatusGsm;
	enum cellular_registration_status registrationStatusGprs;
	enum cellular_registration_status registrationStatusLte;
	uint8_t rssi;
	uint8_t rsrp;
	uint8_t rsrq;
	uint8_t imei[MODEM_RC7620_INFO_IMEI_LEN];
	uint8_t modelId[MODEM_RC7620_INFO_MODEL_LEN];
	uint8_t imsi[MODEM_RC7620_INFO_IMSI_LEN];
	uint8_t iccid[MODEM_RC7620_INFO_ICCID_LEN];
	uint8_t manufacturer[MODEM_RC7620_INFO_MANUFACTURER_LEN];
	uint8_t fwVersion[MODEM_RC7620_INFO_FW_LEN];

	struct modem_ppp *ppp;
	struct net_mgmt_event_callback netMgmtEventCallback;

	enum modem_rc7620_state state;
	const struct device *dev;
	struct k_work_delayable timeoutWork;
	struct k_sem suspendedSem;
	struct k_work eventDispatchWork;
	uint8_t eventBuffer[MODEM_RC7620_EVENT_BUFFER_SIZE];
	struct ring_buf eventRingBuffer;
	struct k_mutex eventLock;
};

struct modem_rc7620_config {
	const struct device *uart;
	struct gpio_dt_spec railGpio;
	struct gpio_dt_spec powerGpio;
	struct gpio_dt_spec resetGpio;
	uint16_t powerOnPulseMs;
	uint16_t powerOffPulseMs;
	uint16_t resetPulseMs;
	uint16_t startupMs;
	uint16_t shutdownMs;
	bool autostarts;
	const struct modem_chat_script *initScript;
	const struct modem_chat_script *periodicScript;
	const struct modem_chat_script *shutdownScript;
};

static bool modem_rc7620_is_registered(struct modem_rc7620_data *data)
{
	return (data->registrationStatusGsm == CELLULAR_REGISTRATION_REGISTERED_HOME) ||
	       (data->registrationStatusGsm == CELLULAR_REGISTRATION_REGISTERED_ROAMING) ||
	       (data->registrationStatusGprs == CELLULAR_REGISTRATION_REGISTERED_HOME) ||
	       (data->registrationStatusGprs == CELLULAR_REGISTRATION_REGISTERED_ROAMING) ||
	       (data->registrationStatusLte == CELLULAR_REGISTRATION_REGISTERED_HOME) ||
	       (data->registrationStatusLte == CELLULAR_REGISTRATION_REGISTERED_ROAMING);
}

static bool modem_rc7620_rail_is_already_enabled(struct modem_rc7620_data *data)
{
	const struct modem_rc7620_config *config = data->dev->config;
	int railState;

	railState = gpio_pin_get_dt(&config->railGpio);
	if (railState < 0) {
		LOG_WRN("failed to read modem rail GPIO (%d)", railState);
		return false;
	}

	return railState != 0;
}

static void modem_rc7620_chat_callback_handler(struct modem_chat *chat,
					      enum modem_chat_script_result result,
					      void *userData);

static int modem_rc7620_update_dial_script(struct modem_rc7620_data *data);
static void modem_rc7620_start_timer(struct modem_rc7620_data *data, k_timeout_t timeout);

static void modem_rc7620_chat_on_simple_response(struct modem_chat *chat, char **argv,
						 uint16_t argc, void *userData)
{
	const struct modem_chat_script_chat *scriptChat = NULL;
	const char *scriptName = "no-script";
	uint16_t scriptIndex = 0U;

	ARG_UNUSED(userData);

	if ((chat != NULL) && (chat->script != NULL)) {
		scriptName = chat->script->name != NULL ? chat->script->name : "unnamed-script";
		scriptIndex = chat->script_chat_it;
		if (scriptIndex < chat->script->script_chats_size) {
			scriptChat = &chat->script->script_chats[scriptIndex];
		}
	}

	if ((argc == 0U) || (argv[0] == NULL)) {
		LOG_INF("AT[%s:%u] < <no response text>", scriptName, scriptIndex);
		return;
	}

	if ((scriptChat != NULL) && (scriptChat->request != NULL) && (scriptChat->request_size != 0U)) {
		LOG_INF("AT[%s:%u] < %s", scriptName, scriptIndex, argv[0]);
	} else {
		LOG_INF("AT[%s:%u] < %s", scriptName, scriptIndex, argv[0]);
	}
}

static int modem_rc7620_copy_profile_value(char *dst, size_t dstSize, const char *src)
{
	int written;

	if ((dst == NULL) || (dstSize == 0U) || (src == NULL)) {
		return -EINVAL;
	}

	written = snprintk(dst, dstSize, "%s", src);
	if ((written < 0) || ((size_t)written >= dstSize)) {
		return -ENOBUFS;
	}

	return 0;
}

static int modem_rc7620_set_profile_locked(struct modem_rc7620_data *data, const char *apn,
					  const char *id, const char *password)
{
	int ret;

	if ((apn == NULL) || (apn[0] == '\0')) {
		return -EINVAL;
	}

	ret = modem_rc7620_copy_profile_value(data->apn, sizeof(data->apn), apn);
	if (ret != 0) {
		return ret;
	}

	ret = modem_rc7620_copy_profile_value(data->id, sizeof(data->id), id != NULL ? id : "");
	if (ret != 0) {
		return ret;
	}

	ret = modem_rc7620_copy_profile_value(data->password, sizeof(data->password),
					      password != NULL ? password : "");
	if (ret != 0) {
		return ret;
	}

	return modem_rc7620_update_dial_script(data);
}

static void modem_rc7620_enter_state(struct modem_rc7620_data *data,
					     enum modem_rc7620_state state);

static void modem_rc7620_delegate_event(struct modem_rc7620_data *data,
					       enum modem_rc7620_event event);

static void modem_rc7620_event_handler(struct modem_rc7620_data *data,
					      enum modem_rc7620_event event);

static void modem_rc7620_bus_pipe_handler(struct modem_pipe *pipe,
					 enum modem_pipe_event event,
					 void *userData)
{
	struct modem_rc7620_data *data = userData;

	ARG_UNUSED(pipe);

	if ((event == MODEM_PIPE_EVENT_OPENED) || (event == MODEM_PIPE_EVENT_CLOSED)) {
		LOG_INF("UART pipe event: %s", modem_rc7620_pipe_event_name(event));
	}

	switch (event) {
	case MODEM_PIPE_EVENT_OPENED:
		modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_BUS_OPENED);
		break;
	case MODEM_PIPE_EVENT_CLOSED:
		modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_BUS_CLOSED);
		break;
	default:
		break;
	}
}

static void modem_rc7620_dlci1_pipe_handler(struct modem_pipe *pipe,
					   enum modem_pipe_event event,
					   void *userData)
{
	struct modem_rc7620_data *data = userData;

	ARG_UNUSED(pipe);

	if (event == MODEM_PIPE_EVENT_OPENED) {
		LOG_INF("DLCI1 pipe event: %s", modem_rc7620_pipe_event_name(event));
		modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_DLCI1_OPENED);
	}
}

static void modem_rc7620_dlci2_pipe_handler(struct modem_pipe *pipe,
					   enum modem_pipe_event event,
					   void *userData)
{
	struct modem_rc7620_data *data = userData;

	ARG_UNUSED(pipe);

	if (event == MODEM_PIPE_EVENT_OPENED) {
		LOG_INF("DLCI2 pipe event: %s", modem_rc7620_pipe_event_name(event));
		modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_DLCI2_OPENED);
	}
}

static void modem_rc7620_chat_callback_handler(struct modem_chat *chat,
					      enum modem_chat_script_result result,
					      void *userData)
{
	struct modem_rc7620_data *data = userData;
	const char *scriptName = "unknown";

	if ((chat != NULL) && (chat->script != NULL) && (chat->script->name != NULL)) {
		scriptName = chat->script->name;
	}

	LOG_INF("Script %s completed with %s in state %s", scriptName,
		modem_rc7620_script_result_name(result),
		modem_rc7620_state_name(data->state));

	if (result == MODEM_CHAT_SCRIPT_RESULT_SUCCESS) {
		modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_SCRIPT_SUCCESS);
	} else {
		modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_SCRIPT_FAILED);
	}
}

static void modem_rc7620_chat_on_csq(struct modem_chat *chat, char **argv, uint16_t argc,
					    void *userData)
{
	struct modem_rc7620_data *data = userData;

	ARG_UNUSED(chat);

	if (argc == 3U) {
		data->rssi = (uint8_t)atoi(argv[1]);
	}
}

static void modem_rc7620_chat_on_cesq(struct modem_chat *chat, char **argv, uint16_t argc,
					     void *userData)
{
	struct modem_rc7620_data *data = userData;

	ARG_UNUSED(chat);

	if (argc == 7U) {
		data->rsrq = (uint8_t)atoi(argv[5]);
		data->rsrp = (uint8_t)atoi(argv[6]);
	}
}

static void modem_rc7620_chat_on_registration(struct modem_chat *chat, char **argv,
					    uint16_t argc, void *userData)
{
	struct modem_rc7620_data *data = userData;
	enum cellular_registration_status registrationStatus;

	ARG_UNUSED(chat);

	if ((argc >= 3U) && (argv[2][0] != '"')) {
		registrationStatus = atoi(argv[2]);
	} else if (argc >= 2U) {
		registrationStatus = atoi(argv[1]);
	} else {
		return;
	}

	if (strcmp(argv[0], "+CREG: ") == 0) {
		data->registrationStatusGsm = registrationStatus;
	} else if (strcmp(argv[0], "+CGREG: ") == 0) {
		data->registrationStatusGprs = registrationStatus;
	} else {
		data->registrationStatusLte = registrationStatus;
	}

	LOG_INF("Registration response %s%d gsm=%d gprs=%d lte=%d", argv[0], registrationStatus,
		data->registrationStatusGsm, data->registrationStatusGprs,
		data->registrationStatusLte);

	if (modem_rc7620_is_registered(data)) {
		modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_REGISTERED);
	} else {
		modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_DEREGISTERED);
	}
}

MODEM_CHAT_MATCH_DEFINE(ok_match, "OK", "", modem_rc7620_chat_on_simple_response);
MODEM_CHAT_MATCH_DEFINE(connect_match, "CONNECT", "", NULL);
MODEM_CHAT_MATCHES_DEFINE(allow_match,
	MODEM_CHAT_MATCH("OK", "", modem_rc7620_chat_on_simple_response),
	MODEM_CHAT_MATCH("ERROR", "", modem_rc7620_chat_on_simple_response));

MODEM_CHAT_MATCH_DEFINE(csq_match, "+CSQ: ", ",", modem_rc7620_chat_on_csq);
MODEM_CHAT_MATCH_DEFINE(cesq_match, "+CESQ: ", ",", modem_rc7620_chat_on_cesq);

MODEM_CHAT_MATCHES_DEFINE(unsol_matches,
	MODEM_CHAT_MATCH("+CREG: ", ",", modem_rc7620_chat_on_registration),
	MODEM_CHAT_MATCH("+CEREG: ", ",", modem_rc7620_chat_on_registration),
	MODEM_CHAT_MATCH("+CGREG: ", ",", modem_rc7620_chat_on_registration));

MODEM_CHAT_MATCHES_DEFINE(abort_matches,
	MODEM_CHAT_MATCH("ERROR", "", modem_rc7620_chat_on_simple_response));

MODEM_CHAT_MATCHES_DEFINE(dial_abort_matches,
	MODEM_CHAT_MATCH("ERROR", "", modem_rc7620_chat_on_simple_response),
	MODEM_CHAT_MATCH("BUSY", "", modem_rc7620_chat_on_simple_response),
	MODEM_CHAT_MATCH("NO ANSWER", "", modem_rc7620_chat_on_simple_response),
	MODEM_CHAT_MATCH("NO CARRIER", "", modem_rc7620_chat_on_simple_response),
	MODEM_CHAT_MATCH("NO DIALTONE", "", modem_rc7620_chat_on_simple_response));

static int modem_rc7620_update_dial_script(struct modem_rc7620_data *data)
{
	int ret;

	ret = snprintk(data->cgdcontCommand, sizeof(data->cgdcontCommand),
		       "AT+CGDCONT=1,\"IP\",\"%s\"", data->apn);
	if ((ret < 0) || (ret >= (int)sizeof(data->cgdcontCommand))) {
		return -ENOBUFS;
	}

	ret = snprintk(data->kcnxcfgCommand, sizeof(data->kcnxcfgCommand),
		       "AT+KCNXCFG=1,\"GPRS\",\"%s\",\"%s\",\"%s\",\"IPV4\"",
		       data->apn, data->id, data->password);
	if ((ret < 0) || (ret >= (int)sizeof(data->kcnxcfgCommand))) {
		return -ENOBUFS;
	}

	data->dialScriptCmds[0] = (struct modem_chat_script_chat){
		.request = (const uint8_t *)data->cgdcontCommand,
		.request_size = (uint16_t)strlen(data->cgdcontCommand),
		.response_matches = &ok_match,
		.response_matches_size = 1,
		.timeout = 0,
	};
	data->dialScriptCmds[1] = (struct modem_chat_script_chat){
		.request = (const uint8_t *)data->kcnxcfgCommand,
		.request_size = (uint16_t)strlen(data->kcnxcfgCommand),
		.response_matches = allow_match,
		.response_matches_size = ARRAY_SIZE(allow_match),
		.timeout = 0,
	};
	data->dialScriptCmds[2] = (struct modem_chat_script_chat)MODEM_CHAT_SCRIPT_CMD_RESP_MULT(
		"AT+WPPP=0", allow_match);
	data->dialScriptCmds[3] = (struct modem_chat_script_chat)MODEM_CHAT_SCRIPT_CMD_RESP_MULT(
		"AT+CFUN=1", allow_match);
	data->dialScriptCmds[4] = (struct modem_chat_script_chat)MODEM_CHAT_SCRIPT_CMD_RESP(
		"ATD*99***1#", connect_match);

	data->dialScript = (struct modem_chat_script){
		.name = "rc7620_prepare_dial_chat_script",
		.script_chats = data->dialScriptCmds,
		.script_chats_size = ARRAY_SIZE(data->dialScriptCmds),
		.abort_matches = dial_abort_matches,
		.abort_matches_size = ARRAY_SIZE(dial_abort_matches),
		.callback = modem_rc7620_chat_callback_handler,
		.timeout = 15,
	};

	return 0;
}

MODEM_CHAT_SCRIPT_CMDS_DEFINE(rc7620_init_chat_script_cmds,
	MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
	MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
	MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
	MODEM_CHAT_SCRIPT_CMD_RESP_NONE("AT", 100),
	MODEM_CHAT_SCRIPT_CMD_RESP_MULT("AT+KSLEEP=2", allow_match),
	MODEM_CHAT_SCRIPT_CMD_RESP_MULT("AT+KSLEEP=2", allow_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CPSMS=0", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CEDRXS=0", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("ATE0", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP_MULT("AT+CFUN=1", allow_match),
	MODEM_CHAT_SCRIPT_CMD_RESP_MULT("AT+CGACT=0", allow_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CFUN=4", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CMEE=1", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG=1", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGREG=1", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CEREG=1", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG?", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CEREG?", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGREG?", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CMUX=0,0,5,31", ok_match));

MODEM_CHAT_SCRIPT_DEFINE(rc7620_init_chat_script, rc7620_init_chat_script_cmds,
	abort_matches, modem_rc7620_chat_callback_handler, 20);

MODEM_CHAT_SCRIPT_CMDS_DEFINE(rc7620_periodic_chat_script_cmds,
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG?", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CEREG?", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CGREG?", ok_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CSQ", csq_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match));

MODEM_CHAT_SCRIPT_DEFINE(rc7620_periodic_chat_script, rc7620_periodic_chat_script_cmds,
	abort_matches, modem_rc7620_chat_callback_handler, 4);

MODEM_CHAT_SCRIPT_CMDS_DEFINE(get_signal_csq_chat_script_cmds,
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CSQ", csq_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match));

MODEM_CHAT_SCRIPT_DEFINE(get_signal_csq_chat_script, get_signal_csq_chat_script_cmds,
	abort_matches, modem_rc7620_chat_callback_handler, 2);

MODEM_CHAT_SCRIPT_CMDS_DEFINE(get_signal_cesq_chat_script_cmds,
	MODEM_CHAT_SCRIPT_CMD_RESP("AT+CESQ", cesq_match),
	MODEM_CHAT_SCRIPT_CMD_RESP("", ok_match));

MODEM_CHAT_SCRIPT_DEFINE(get_signal_cesq_chat_script, get_signal_cesq_chat_script_cmds,
	abort_matches, modem_rc7620_chat_callback_handler, 2);

static void modem_rc7620_start_timer(struct modem_rc7620_data *data, k_timeout_t timeout)
{
	k_work_schedule(&data->timeoutWork, timeout);
}

static void modem_rc7620_stop_timer(struct modem_rc7620_data *data)
{
	k_work_cancel_delayable(&data->timeoutWork);
}

static void modem_rc7620_timeout_handler(struct k_work *work)
{
	struct k_work_delayable *delayable = k_work_delayable_from_work(work);
	struct modem_rc7620_data *data = CONTAINER_OF(delayable, struct modem_rc7620_data,
						     timeoutWork);

	modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_TIMEOUT);
}

static void modem_rc7620_event_dispatch_handler(struct k_work *work)
{
	struct modem_rc7620_data *data = CONTAINER_OF(work, struct modem_rc7620_data,
					      eventDispatchWork);
	uint8_t events[sizeof(data->eventBuffer)];
	uint32_t count;

	k_mutex_lock(&data->eventLock, K_FOREVER);
	count = ring_buf_get(&data->eventRingBuffer, events, sizeof(events));
	k_mutex_unlock(&data->eventLock);

	for (uint32_t index = 0U; index < count; ++index) {
		LOG_INF("Handling event %s in state %s", 
			modem_rc7620_event_name((enum modem_rc7620_event)events[index]),
			modem_rc7620_state_name(data->state));
		modem_rc7620_event_handler(data, (enum modem_rc7620_event)events[index]);
	}
}

static void modem_rc7620_delegate_event(struct modem_rc7620_data *data,
					       enum modem_rc7620_event event)
{
	uint32_t written;

	k_mutex_lock(&data->eventLock, K_FOREVER);
	written = ring_buf_put(&data->eventRingBuffer, (uint8_t *)&event, 1U);
	k_mutex_unlock(&data->eventLock);

	if (written != 1U) {
		LOG_WRN("dropping modem event %u", (uint32_t)event);
		return;
	}

	LOG_INF("Queued event %s while in state %s", modem_rc7620_event_name(event),
		modem_rc7620_state_name(data->state));

	k_work_submit(&data->eventDispatchWork);
}

static void modem_rc7620_begin_power_off_pulse(struct modem_rc7620_data *data)
{
	modem_pipe_close_async(data->uartPipe);
	modem_rc7620_enter_state(data, MODEM_RC7620_STATE_POWER_OFF_PULSE);
}

static int modem_rc7620_on_idle_state_enter(struct modem_rc7620_data *data)
{
	const struct modem_rc7620_config *config = data->dev->config;

	gpio_pin_set_dt(&config->powerGpio, 0);
	gpio_pin_set_dt(&config->resetGpio, 0);
	gpio_pin_set_dt(&config->railGpio, 0);

	modem_chat_release(&data->chat);
	modem_ppp_release(data->ppp);
	modem_cmux_release(&data->cmux);
	modem_pipe_close_async(data->uartPipe);
	k_sem_give(&data->suspendedSem);

	return 0;
}

static void modem_rc7620_idle_event_handler(struct modem_rc7620_data *data,
					   enum modem_rc7620_event event)
{
	const struct modem_rc7620_config *config = data->dev->config;

	switch (event) {
	case MODEM_RC7620_EVENT_RESUME:
		if (modem_rc7620_rail_is_already_enabled(data)) {
			LOG_INF("Modem rail already enabled, skipping power-on pulse");
			modem_rc7620_enter_state(data, MODEM_RC7620_STATE_AWAIT_POWER_ON);
		} else if (config->autostarts) {
			modem_rc7620_enter_state(data, MODEM_RC7620_STATE_AWAIT_POWER_ON);
		} else {
			modem_rc7620_enter_state(data, MODEM_RC7620_STATE_POWER_ON_PULSE);
		}
		break;
	case MODEM_RC7620_EVENT_SUSPEND:
		k_sem_give(&data->suspendedSem);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_idle_state_leave(struct modem_rc7620_data *data)
{
	k_sem_take(&data->suspendedSem, K_NO_WAIT);
	return 0;
}

static int modem_rc7620_on_reset_pulse_state_enter(struct modem_rc7620_data *data)
{
	const struct modem_rc7620_config *config = data->dev->config;

	gpio_pin_set_dt(&config->resetGpio, 1);
	modem_rc7620_start_timer(data, K_MSEC(config->resetPulseMs));
	return 0;
}

static void modem_rc7620_reset_pulse_event_handler(struct modem_rc7620_data *data,
					   enum modem_rc7620_event event)
{
	switch (event) {
	case MODEM_RC7620_EVENT_TIMEOUT:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_AWAIT_POWER_ON);
		break;
	case MODEM_RC7620_EVENT_SUSPEND:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_reset_pulse_state_leave(struct modem_rc7620_data *data)
{
	const struct modem_rc7620_config *config = data->dev->config;

	modem_rc7620_stop_timer(data);
	gpio_pin_set_dt(&config->resetGpio, 0);
	return 0;
}

static int modem_rc7620_on_power_on_pulse_state_enter(struct modem_rc7620_data *data)
{
	const struct modem_rc7620_config *config = data->dev->config;

	gpio_pin_set_dt(&config->railGpio, 1);
	k_msleep(MODEM_RC7620_RAIL_SETTLE_MS);
	gpio_pin_set_dt(&config->resetGpio, 0);
	gpio_pin_set_dt(&config->powerGpio, 1);
	modem_rc7620_start_timer(data, K_MSEC(config->powerOnPulseMs));
	return 0;
}

static void modem_rc7620_power_on_pulse_event_handler(struct modem_rc7620_data *data,
					      enum modem_rc7620_event event)
{
	switch (event) {
	case MODEM_RC7620_EVENT_TIMEOUT:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_AWAIT_POWER_ON);
		break;
	case MODEM_RC7620_EVENT_SUSPEND:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_power_on_pulse_state_leave(struct modem_rc7620_data *data)
{
	const struct modem_rc7620_config *config = data->dev->config;

	modem_rc7620_stop_timer(data);
	gpio_pin_set_dt(&config->powerGpio, 0);
	return 0;
}

static int modem_rc7620_on_await_power_on_state_enter(struct modem_rc7620_data *data)
{
	const struct modem_rc7620_config *config = data->dev->config;

	modem_rc7620_start_timer(data, K_MSEC(config->startupMs));
	return 0;
}

static void modem_rc7620_await_power_on_event_handler(struct modem_rc7620_data *data,
					     enum modem_rc7620_event event)
{
	switch (event) {
	case MODEM_RC7620_EVENT_TIMEOUT:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_RUN_INIT_SCRIPT);
		break;
	case MODEM_RC7620_EVENT_SUSPEND:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_await_power_on_state_leave(struct modem_rc7620_data *data)
{
	modem_rc7620_stop_timer(data);
	return 0;
}

static int modem_rc7620_on_run_init_script_state_enter(struct modem_rc7620_data *data)
{
	int ret;

	data->registrationStatusGsm = 0;
	data->registrationStatusGprs = 0;
	data->registrationStatusLte = 0;
	data->cmdPipe = NULL;

	modem_pipe_attach(data->uartPipe, modem_rc7620_bus_pipe_handler, data);
	ret = modem_pipe_open_async(data->uartPipe);

	return ret;
}

static void modem_rc7620_run_init_script_event_handler(struct modem_rc7620_data *data,
					      enum modem_rc7620_event event)
{
	uint8_t *linkAddress;
	uint8_t linkAddressLen;
	uint8_t imeiLen;
	int err;

	switch (event) {
	case MODEM_RC7620_EVENT_BUS_OPENED:
		LOG_INF("RC7620 UART open, running init script");
		modem_chat_attach(&data->chat, data->uartPipe);
		modem_rc7620_run_script_async(data->state, &data->chat,
					 ((const struct modem_rc7620_config *)data->dev->config)->initScript);
		break;
	case MODEM_RC7620_EVENT_SCRIPT_SUCCESS:
		LOG_INF("RC7620 init script complete");
		if (data->imei[0] != '\0') {
			imeiLen = MODEM_RC7620_INFO_IMEI_LEN - 1U;
			linkAddressLen = MIN(NET_LINK_ADDR_MAX_LENGTH, imeiLen);
			linkAddress = data->imei + (imeiLen - linkAddressLen);
			err = net_if_set_link_addr(modem_ppp_get_iface(data->ppp), linkAddress,
						     linkAddressLen, NET_LINK_UNKNOWN);
			if (err < 0) {
				LOG_WRN("failed to set PPP link address (%d)", err);
			}
		}
		modem_chat_release(&data->chat);
		modem_pipe_attach(data->uartPipe, modem_rc7620_bus_pipe_handler, data);
		modem_pipe_close_async(data->uartPipe);
		break;
	case MODEM_RC7620_EVENT_BUS_CLOSED:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_CONNECT_CMUX);
		break;
	case MODEM_RC7620_EVENT_SCRIPT_FAILED:
		LOG_WRN("RC7620 init script failed, resetting modem");
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_RESET_PULSE);
		break;
	case MODEM_RC7620_EVENT_SUSPEND:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_connect_cmux_state_enter(struct modem_rc7620_data *data)
{
	modem_rc7620_start_timer(data, K_MSEC(MODEM_RC7620_CMUX_DELAY_MS));
	return 0;
}

static void modem_rc7620_connect_cmux_event_handler(struct modem_rc7620_data *data,
					   enum modem_rc7620_event event)
{
	switch (event) {
	case MODEM_RC7620_EVENT_TIMEOUT:
		modem_pipe_attach(data->uartPipe, modem_rc7620_bus_pipe_handler, data);
		modem_pipe_open_async(data->uartPipe);
		break;
	case MODEM_RC7620_EVENT_BUS_OPENED:
		modem_cmux_attach(&data->cmux, data->uartPipe);
		modem_cmux_connect_async(&data->cmux);
		break;
	case MODEM_RC7620_EVENT_CMUX_CONNECTED:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_OPEN_DLCI1);
		break;
	case MODEM_RC7620_EVENT_SUSPEND:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_connect_cmux_state_leave(struct modem_rc7620_data *data)
{
	modem_rc7620_stop_timer(data);
	return 0;
}

static int modem_rc7620_on_open_dlci1_state_enter(struct modem_rc7620_data *data)
{
	modem_pipe_attach(data->dlci1Pipe, modem_rc7620_dlci1_pipe_handler, data);
	return modem_pipe_open_async(data->dlci1Pipe);
}

static void modem_rc7620_open_dlci1_event_handler(struct modem_rc7620_data *data,
					 enum modem_rc7620_event event)
{
	switch (event) {
	case MODEM_RC7620_EVENT_DLCI1_OPENED:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_OPEN_DLCI2);
		break;
	case MODEM_RC7620_EVENT_SUSPEND:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_open_dlci1_state_leave(struct modem_rc7620_data *data)
{
	modem_pipe_release(data->dlci1Pipe);
	return 0;
}

static int modem_rc7620_on_open_dlci2_state_enter(struct modem_rc7620_data *data)
{
	modem_pipe_attach(data->dlci2Pipe, modem_rc7620_dlci2_pipe_handler, data);
	return modem_pipe_open_async(data->dlci2Pipe);
}

static void modem_rc7620_open_dlci2_event_handler(struct modem_rc7620_data *data,
					 enum modem_rc7620_event event)
{
	switch (event) {
	case MODEM_RC7620_EVENT_DLCI2_OPENED:
		LOG_INF("RC7620 CMUX channels open, starting PPP dial");
		data->cmdPipe = data->dlci2Pipe;
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_RUN_DIAL_SCRIPT);
		break;
	case MODEM_RC7620_EVENT_SUSPEND:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_open_dlci2_state_leave(struct modem_rc7620_data *data)
{
	modem_pipe_release(data->dlci2Pipe);
	return 0;
}

static int modem_rc7620_on_run_dial_script_state_enter(struct modem_rc7620_data *data)
{
	modem_rc7620_start_timer(data, K_MSEC(MODEM_RC7620_DIAL_DELAY_MS));
	return 0;
}

static void modem_rc7620_run_dial_script_event_handler(struct modem_rc7620_data *data,
					      enum modem_rc7620_event event)
{
	switch (event) {
	case MODEM_RC7620_EVENT_TIMEOUT:
		modem_chat_release(&data->chat);
		modem_chat_attach(&data->chat, data->dlci1Pipe);
		modem_rc7620_run_script_async(data->state, &data->chat, &data->dialScript);
		break;
	case MODEM_RC7620_EVENT_SCRIPT_SUCCESS:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_AWAIT_REGISTERED);
		break;
	case MODEM_RC7620_EVENT_SCRIPT_FAILED:
		LOG_WRN("PPP dial failed, retrying after backoff");
		modem_rc7620_start_timer(data, MODEM_RC7620_PERIODIC_TIMEOUT);
		break;
	case MODEM_RC7620_EVENT_SUSPEND:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_run_dial_script_state_leave(struct modem_rc7620_data *data)
{
	modem_rc7620_stop_timer(data);
	modem_chat_release(&data->chat);
	return 0;
}

static int modem_rc7620_on_await_registered_state_enter(struct modem_rc7620_data *data)
{
	if (modem_ppp_attach(data->ppp, data->dlci1Pipe) < 0) {
		return -EAGAIN;
	}

	data->cmdPipe = data->dlci2Pipe;
	modem_rc7620_start_timer(data, MODEM_RC7620_PERIODIC_TIMEOUT);
	return modem_chat_attach(&data->chat, data->dlci2Pipe);
}

static void modem_rc7620_await_registered_event_handler(struct modem_rc7620_data *data,
					      enum modem_rc7620_event event)
{
	const struct modem_rc7620_config *config = data->dev->config;

	switch (event) {
	case MODEM_RC7620_EVENT_SCRIPT_SUCCESS:
	case MODEM_RC7620_EVENT_SCRIPT_FAILED:
		modem_rc7620_start_timer(data, MODEM_RC7620_PERIODIC_TIMEOUT);
		break;
	case MODEM_RC7620_EVENT_TIMEOUT:
		modem_rc7620_run_script_async(data->state, &data->chat, config->periodicScript);
		break;
	case MODEM_RC7620_EVENT_REGISTERED:
		LOG_INF("Network registered, enabling PPP carrier");
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_CARRIER_ON);
		break;
	case MODEM_RC7620_EVENT_PPP_DEAD:
		LOG_WRN("PPP died while waiting for registration, restarting modem");
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	case MODEM_RC7620_EVENT_SUSPEND:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_await_registered_state_leave(struct modem_rc7620_data *data)
{
	modem_rc7620_stop_timer(data);
	modem_chat_release(&data->chat);
	return 0;
}

static int modem_rc7620_on_carrier_on_state_enter(struct modem_rc7620_data *data)
{
	LOG_INF("PPP carrier on");
	net_if_carrier_on(modem_ppp_get_iface(data->ppp));
	return 0;
}

static void modem_rc7620_carrier_on_event_handler(struct modem_rc7620_data *data,
					 enum modem_rc7620_event event)
{
	switch (event) {
	case MODEM_RC7620_EVENT_PPP_DEAD:
		LOG_WRN("PPP link died");
		net_if_carrier_off(modem_ppp_get_iface(data->ppp));
		modem_ppp_release(data->ppp);
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	case MODEM_RC7620_EVENT_SUSPEND:
		LOG_INF("PPP carrier off for suspend");
		net_if_carrier_off(modem_ppp_get_iface(data->ppp));
		modem_ppp_release(data->ppp);
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_carrier_on_state_leave(struct modem_rc7620_data *data)
{
	ARG_UNUSED(data);
	return 0;
}

static int modem_rc7620_on_dormant_state_enter(struct modem_rc7620_data *data)
{
	net_if_dormant_on(modem_ppp_get_iface(data->ppp));
	return 0;
}

static void modem_rc7620_dormant_event_handler(struct modem_rc7620_data *data,
					 enum modem_rc7620_event event)
{
	switch (event) {
	case MODEM_RC7620_EVENT_PPP_DEAD:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	case MODEM_RC7620_EVENT_REGISTERED:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_CARRIER_ON);
		break;
	case MODEM_RC7620_EVENT_SUSPEND:
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_INIT_POWER_OFF);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_dormant_state_leave(struct modem_rc7620_data *data)
{
	net_if_carrier_off(modem_ppp_get_iface(data->ppp));
	modem_chat_release(&data->chat);
	modem_ppp_release(data->ppp);
	net_if_dormant_off(modem_ppp_get_iface(data->ppp));
	return 0;
}

static int modem_rc7620_on_init_power_off_state_enter(struct modem_rc7620_data *data)
{
	modem_rc7620_start_timer(data, K_MSEC(2000));
	return 0;
}

static void modem_rc7620_init_power_off_event_handler(struct modem_rc7620_data *data,
					     enum modem_rc7620_event event)
{
	const struct modem_rc7620_config *config = data->dev->config;

	switch (event) {
	case MODEM_RC7620_EVENT_TIMEOUT:
		if ((config->shutdownScript != NULL) && (data->cmdPipe != NULL)) {
			modem_rc7620_enter_state(data, MODEM_RC7620_STATE_RUN_SHUTDOWN_SCRIPT);
		} else {
			modem_rc7620_begin_power_off_pulse(data);
		}
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_init_power_off_state_leave(struct modem_rc7620_data *data)
{
	modem_rc7620_stop_timer(data);
	modem_chat_release(&data->chat);
	modem_ppp_release(data->ppp);
	return 0;
}

static int modem_rc7620_on_run_shutdown_script_state_enter(struct modem_rc7620_data *data)
{
	const struct modem_rc7620_config *config = data->dev->config;

	modem_chat_attach(&data->chat, data->cmdPipe);
	return modem_chat_run_script_async(&data->chat, config->shutdownScript);
}

static void modem_rc7620_run_shutdown_script_event_handler(struct modem_rc7620_data *data,
					      enum modem_rc7620_event event)
{
	switch (event) {
	case MODEM_RC7620_EVENT_SCRIPT_FAILED:
		data->cmdPipe = NULL;
		modem_rc7620_begin_power_off_pulse(data);
		break;
	case MODEM_RC7620_EVENT_SCRIPT_SUCCESS:
		modem_pipe_close_async(data->uartPipe);
		data->cmdPipe = NULL;
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_IDLE);
		break;
	default:
		break;
	}
}

static int modem_rc7620_on_run_shutdown_script_state_leave(struct modem_rc7620_data *data)
{
	modem_chat_release(&data->chat);
	return 0;
}

static int modem_rc7620_on_power_off_pulse_state_enter(struct modem_rc7620_data *data)
{
	const struct modem_rc7620_config *config = data->dev->config;

	data->cmdPipe = NULL;
	gpio_pin_set_dt(&config->powerGpio, 1);
	modem_rc7620_start_timer(data, K_MSEC(config->powerOffPulseMs));
	return 0;
}

static void modem_rc7620_power_off_pulse_event_handler(struct modem_rc7620_data *data,
					       enum modem_rc7620_event event)
{
	if (event == MODEM_RC7620_EVENT_TIMEOUT) {
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_AWAIT_POWER_OFF);
	}
}

static int modem_rc7620_on_power_off_pulse_state_leave(struct modem_rc7620_data *data)
{
	const struct modem_rc7620_config *config = data->dev->config;

	modem_rc7620_stop_timer(data);
	gpio_pin_set_dt(&config->powerGpio, 0);
	return 0;
}

static int modem_rc7620_on_await_power_off_state_enter(struct modem_rc7620_data *data)
{
	const struct modem_rc7620_config *config = data->dev->config;

	modem_rc7620_start_timer(data, K_MSEC(config->shutdownMs));
	return 0;
}

static void modem_rc7620_await_power_off_event_handler(struct modem_rc7620_data *data,
					      enum modem_rc7620_event event)
{
	const struct modem_rc7620_config *config = data->dev->config;

	if (event == MODEM_RC7620_EVENT_TIMEOUT) {
		gpio_pin_set_dt(&config->railGpio, 0);
		modem_rc7620_enter_state(data, MODEM_RC7620_STATE_IDLE);
	}
}

static int modem_rc7620_on_await_power_off_state_leave(struct modem_rc7620_data *data)
{
	modem_rc7620_stop_timer(data);
	return 0;
}

static int modem_rc7620_on_state_enter(struct modem_rc7620_data *data)
{
	switch (data->state) {
	case MODEM_RC7620_STATE_IDLE:
		return modem_rc7620_on_idle_state_enter(data);
	case MODEM_RC7620_STATE_RESET_PULSE:
		return modem_rc7620_on_reset_pulse_state_enter(data);
	case MODEM_RC7620_STATE_POWER_ON_PULSE:
		return modem_rc7620_on_power_on_pulse_state_enter(data);
	case MODEM_RC7620_STATE_AWAIT_POWER_ON:
		return modem_rc7620_on_await_power_on_state_enter(data);
	case MODEM_RC7620_STATE_RUN_INIT_SCRIPT:
		return modem_rc7620_on_run_init_script_state_enter(data);
	case MODEM_RC7620_STATE_CONNECT_CMUX:
		return modem_rc7620_on_connect_cmux_state_enter(data);
	case MODEM_RC7620_STATE_OPEN_DLCI1:
		return modem_rc7620_on_open_dlci1_state_enter(data);
	case MODEM_RC7620_STATE_OPEN_DLCI2:
		return modem_rc7620_on_open_dlci2_state_enter(data);
	case MODEM_RC7620_STATE_RUN_DIAL_SCRIPT:
		return modem_rc7620_on_run_dial_script_state_enter(data);
	case MODEM_RC7620_STATE_AWAIT_REGISTERED:
		return modem_rc7620_on_await_registered_state_enter(data);
	case MODEM_RC7620_STATE_CARRIER_ON:
		return modem_rc7620_on_carrier_on_state_enter(data);
	case MODEM_RC7620_STATE_DORMANT:
		return modem_rc7620_on_dormant_state_enter(data);
	case MODEM_RC7620_STATE_INIT_POWER_OFF:
		return modem_rc7620_on_init_power_off_state_enter(data);
	case MODEM_RC7620_STATE_RUN_SHUTDOWN_SCRIPT:
		return modem_rc7620_on_run_shutdown_script_state_enter(data);
	case MODEM_RC7620_STATE_POWER_OFF_PULSE:
		return modem_rc7620_on_power_off_pulse_state_enter(data);
	case MODEM_RC7620_STATE_AWAIT_POWER_OFF:
		return modem_rc7620_on_await_power_off_state_enter(data);
	default:
		return 0;
	}
}

static int modem_rc7620_on_state_leave(struct modem_rc7620_data *data)
{
	switch (data->state) {
	case MODEM_RC7620_STATE_IDLE:
		return modem_rc7620_on_idle_state_leave(data);
	case MODEM_RC7620_STATE_RESET_PULSE:
		return modem_rc7620_on_reset_pulse_state_leave(data);
	case MODEM_RC7620_STATE_POWER_ON_PULSE:
		return modem_rc7620_on_power_on_pulse_state_leave(data);
	case MODEM_RC7620_STATE_AWAIT_POWER_ON:
		return modem_rc7620_on_await_power_on_state_leave(data);
	case MODEM_RC7620_STATE_CONNECT_CMUX:
		return modem_rc7620_on_connect_cmux_state_leave(data);
	case MODEM_RC7620_STATE_OPEN_DLCI1:
		return modem_rc7620_on_open_dlci1_state_leave(data);
	case MODEM_RC7620_STATE_OPEN_DLCI2:
		return modem_rc7620_on_open_dlci2_state_leave(data);
	case MODEM_RC7620_STATE_RUN_DIAL_SCRIPT:
		return modem_rc7620_on_run_dial_script_state_leave(data);
	case MODEM_RC7620_STATE_AWAIT_REGISTERED:
		return modem_rc7620_on_await_registered_state_leave(data);
	case MODEM_RC7620_STATE_CARRIER_ON:
		return modem_rc7620_on_carrier_on_state_leave(data);
	case MODEM_RC7620_STATE_DORMANT:
		return modem_rc7620_on_dormant_state_leave(data);
	case MODEM_RC7620_STATE_INIT_POWER_OFF:
		return modem_rc7620_on_init_power_off_state_leave(data);
	case MODEM_RC7620_STATE_RUN_SHUTDOWN_SCRIPT:
		return modem_rc7620_on_run_shutdown_script_state_leave(data);
	case MODEM_RC7620_STATE_POWER_OFF_PULSE:
		return modem_rc7620_on_power_off_pulse_state_leave(data);
	case MODEM_RC7620_STATE_AWAIT_POWER_OFF:
		return modem_rc7620_on_await_power_off_state_leave(data);
	default:
		return 0;
	}
}

static void modem_rc7620_enter_state(struct modem_rc7620_data *data,
					     enum modem_rc7620_state state)
{
	int ret;

	ret = modem_rc7620_on_state_leave(data);
	if (ret < 0) {
		LOG_WRN("failed to leave state %d (%d)", data->state, ret);
		return;
	}

	LOG_INF("State transition: %s -> %s", modem_rc7620_state_name(data->state),
		modem_rc7620_state_name(state));
	data->state = state;
	ret = modem_rc7620_on_state_enter(data);
	if (ret < 0) {
		LOG_WRN("failed to enter state %d (%d)", data->state, ret);
	}
}

static void modem_rc7620_event_handler(struct modem_rc7620_data *data,
					      enum modem_rc7620_event event)
{
	switch (data->state) {
	case MODEM_RC7620_STATE_IDLE:
		modem_rc7620_idle_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_RESET_PULSE:
		modem_rc7620_reset_pulse_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_POWER_ON_PULSE:
		modem_rc7620_power_on_pulse_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_AWAIT_POWER_ON:
		modem_rc7620_await_power_on_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_RUN_INIT_SCRIPT:
		modem_rc7620_run_init_script_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_CONNECT_CMUX:
		modem_rc7620_connect_cmux_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_OPEN_DLCI1:
		modem_rc7620_open_dlci1_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_OPEN_DLCI2:
		modem_rc7620_open_dlci2_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_RUN_DIAL_SCRIPT:
		modem_rc7620_run_dial_script_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_AWAIT_REGISTERED:
		modem_rc7620_await_registered_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_CARRIER_ON:
		modem_rc7620_carrier_on_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_DORMANT:
		modem_rc7620_dormant_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_INIT_POWER_OFF:
		modem_rc7620_init_power_off_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_RUN_SHUTDOWN_SCRIPT:
		modem_rc7620_run_shutdown_script_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_POWER_OFF_PULSE:
		modem_rc7620_power_off_pulse_event_handler(data, event);
		break;
	case MODEM_RC7620_STATE_AWAIT_POWER_OFF:
		modem_rc7620_await_power_off_event_handler(data, event);
		break;
	default:
		break;
	}
}

static void modem_rc7620_cmux_handler(struct modem_cmux *cmux,
					 enum modem_cmux_event event,
					 void *userData)
{
	struct modem_rc7620_data *data = userData;

	ARG_UNUSED(cmux);

	if (event == MODEM_CMUX_EVENT_CONNECTED) {
		modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_CMUX_CONNECTED);
	}
}

static inline int modem_rc7620_parse_csq_rssi(uint8_t rssi, int16_t *value)
{
	if (rssi == CSQ_RSSI_UNKNOWN) {
		return -EINVAL;
	}

	*value = (int16_t)CSQ_RSSI_TO_DB(rssi);
	return 0;
}

static inline int modem_rc7620_parse_cesq_rsrp(uint8_t rsrp, int16_t *value)
{
	if (rsrp == CESQ_RSRP_UNKNOWN) {
		return -EINVAL;
	}

	*value = (int16_t)CESQ_RSRP_TO_DB(rsrp);
	return 0;
}

static inline int modem_rc7620_parse_cesq_rsrq(uint8_t rsrq, int16_t *value)
{
	if (rsrq == CESQ_RSRQ_UNKNOWN) {
		return -EINVAL;
	}

	*value = (int16_t)CESQ_RSRQ_TO_DB(rsrq);
	return 0;
}

static int modem_rc7620_get_signal(const struct device *dev,
					 const enum cellular_signal_type type,
					 int16_t *value)
{
	struct modem_rc7620_data *data = dev->data;
	int ret;

	if (data->state != MODEM_RC7620_STATE_AWAIT_REGISTERED) {
		return -ENODATA;
	}

	switch (type) {
	case CELLULAR_SIGNAL_RSSI:
		ret = modem_chat_run_script(&data->chat, &get_signal_csq_chat_script);
		if (ret < 0) {
			return ret;
		}
		return modem_rc7620_parse_csq_rssi(data->rssi, value);
	case CELLULAR_SIGNAL_RSRP:
	case CELLULAR_SIGNAL_RSRQ:
		ret = modem_chat_run_script(&data->chat, &get_signal_cesq_chat_script);
		if (ret < 0) {
			return ret;
		}
		if (type == CELLULAR_SIGNAL_RSRP) {
			return modem_rc7620_parse_cesq_rsrp(data->rsrp, value);
		}
		return modem_rc7620_parse_cesq_rsrq(data->rsrq, value);
	default:
		return -ENOTSUP;
	}
}

static int modem_rc7620_get_modem_info(const struct device *dev,
					 enum cellular_modem_info_type type,
					 char *info, size_t size)
{
	struct modem_rc7620_data *data = dev->data;

	switch (type) {
	case CELLULAR_MODEM_INFO_IMEI:
		snprintk(info, size, "%s", data->imei);
		return 0;
	case CELLULAR_MODEM_INFO_MODEL_ID:
		snprintk(info, size, "%s", data->modelId);
		return 0;
	case CELLULAR_MODEM_INFO_MANUFACTURER:
		snprintk(info, size, "%s", data->manufacturer);
		return 0;
	case CELLULAR_MODEM_INFO_FW_VERSION:
		snprintk(info, size, "%s", data->fwVersion);
		return 0;
	case CELLULAR_MODEM_INFO_SIM_IMSI:
		snprintk(info, size, "%s", data->imsi);
		return 0;
	case CELLULAR_MODEM_INFO_SIM_ICCID:
		snprintk(info, size, "%s", data->iccid);
		return 0;
	default:
		return -ENODATA;
	}
}

static int modem_rc7620_get_registration_status(const struct device *dev,
					       enum cellular_access_technology tech,
					       enum cellular_registration_status *status)
{
	struct modem_rc7620_data *data = dev->data;

	switch (tech) {
	case CELLULAR_ACCESS_TECHNOLOGY_GSM:
		*status = data->registrationStatusGsm;
		return 0;
	case CELLULAR_ACCESS_TECHNOLOGY_GPRS:
	case CELLULAR_ACCESS_TECHNOLOGY_UMTS:
	case CELLULAR_ACCESS_TECHNOLOGY_EDGE:
		*status = data->registrationStatusGprs;
		return 0;
	case CELLULAR_ACCESS_TECHNOLOGY_LTE:
	case CELLULAR_ACCESS_TECHNOLOGY_LTE_CAT_M1:
	case CELLULAR_ACCESS_TECHNOLOGY_LTE_CAT_M2:
	case CELLULAR_ACCESS_TECHNOLOGY_NB_IOT:
		*status = data->registrationStatusLte;
		return 0;
	default:
		return -ENODATA;
	}
}

static DEVICE_API(cellular, modem_rc7620_api) = {
	.get_signal = modem_rc7620_get_signal,
	.get_modem_info = modem_rc7620_get_modem_info,
	.get_registration_status = modem_rc7620_get_registration_status,
};

static int modem_rc7620_pm_action(const struct device *dev, enum pm_device_action action)
{
	struct modem_rc7620_data *data = dev->data;

	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_RESUME);
		return 0;
	case PM_DEVICE_ACTION_SUSPEND:
		modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_SUSPEND);
		return k_sem_take(&data->suspendedSem, K_SECONDS(30));
	default:
		return -ENOTSUP;
	}
}

static void modem_rc7620_net_mgmt_event_handler(struct net_mgmt_event_callback *callback,
						 uint64_t event,
						 struct net_if *iface)
{
	struct modem_rc7620_data *data = CONTAINER_OF(callback, struct modem_rc7620_data,
						      netMgmtEventCallback);

	ARG_UNUSED(iface);

	LOG_INF("PPP net event: %s", modem_rc7620_ppp_event_name(event));

	if (event == NET_EVENT_PPP_PHASE_DEAD) {
		modem_rc7620_delegate_event(data, MODEM_RC7620_EVENT_PPP_DEAD);
	}
}

static int modem_rc7620_init(const struct device *dev)
{
	struct modem_rc7620_data *data = dev->data;
	struct modem_rc7620_config *config = (struct modem_rc7620_config *)dev->config;
	int ret;
	const struct modem_backend_uart_config uartBackendConfig = {
		.uart = config->uart,
		.receive_buf = data->uartRxBuffer,
		.receive_buf_size = ARRAY_SIZE(data->uartRxBuffer),
		.transmit_buf = data->uartTxBuffer,
		.transmit_buf_size = ARRAY_SIZE(data->uartTxBuffer),
	};
	const struct modem_cmux_config cmuxConfig = {
		.callback = modem_rc7620_cmux_handler,
		.user_data = data,
		.receive_buf = data->cmuxRxBuffer,
		.receive_buf_size = ARRAY_SIZE(data->cmuxRxBuffer),
		.transmit_buf = data->cmuxTxBuffer,
		.transmit_buf_size = ARRAY_SIZE(data->cmuxTxBuffer),
	};
	const struct modem_cmux_dlci_config dlci1Config = {
		.dlci_address = 1,
		.receive_buf = data->dlci1RxBuffer,
		.receive_buf_size = ARRAY_SIZE(data->dlci1RxBuffer),
	};
	const struct modem_cmux_dlci_config dlci2Config = {
		.dlci_address = 2,
		.receive_buf = data->dlci2RxBuffer,
		.receive_buf_size = ARRAY_SIZE(data->dlci2RxBuffer),
	};
	const struct modem_chat_config chatConfig = {
		.user_data = data,
		.receive_buf = data->chatRxBuffer,
		.receive_buf_size = ARRAY_SIZE(data->chatRxBuffer),
		.delimiter = data->chatDelimiter,
		.delimiter_size = strlen((const char *)data->chatDelimiter),
		.filter = data->chatFilter,
		.filter_size = strlen((const char *)data->chatFilter),
		.argv = data->chatArgv,
		.argv_size = ARRAY_SIZE(data->chatArgv),
		.unsol_matches = unsol_matches,
		.unsol_matches_size = ARRAY_SIZE(unsol_matches),
	};

	data->dev = dev;
	k_work_init_delayable(&data->timeoutWork, modem_rc7620_timeout_handler);
	k_work_init(&data->eventDispatchWork, modem_rc7620_event_dispatch_handler);
	ring_buf_init(&data->eventRingBuffer, sizeof(data->eventBuffer), data->eventBuffer);
	k_mutex_init(&data->eventLock);
	k_mutex_init(&data->profileLock);
	k_sem_init(&data->suspendedSem, 0, 1);

	gpio_pin_configure_dt(&config->railGpio, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&config->powerGpio, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&config->resetGpio, GPIO_OUTPUT_INACTIVE);

	data->uartPipe = modem_backend_uart_init(&data->uartBackend, &uartBackendConfig);
	data->cmdPipe = NULL;

	modem_cmux_init(&data->cmux, &cmuxConfig);
	data->dlci1Pipe = modem_cmux_dlci_init(&data->cmux, &data->dlci1, &dlci1Config);
	data->dlci2Pipe = modem_cmux_dlci_init(&data->cmux, &data->dlci2, &dlci2Config);
	modem_chat_init(&data->chat, &chatConfig);
	data->apn[0] = '\0';
	data->id[0] = '\0';
	data->password[0] = '\0';
	if (CONFIG_CONTROL_APN[0] != '\0') {
		ret = modem_rc7620_set_profile_locked(data,
					     CONFIG_CONTROL_APN,
					     CONFIG_CONTROL_APN_USERNAME,
					     CONFIG_CONTROL_APN_PASSWORD);
		if (ret != 0) {
			LOG_ERR("failed to initialize modem profile (%d)", ret);
			return ret;
		}
	} else {
		LOG_WRN("CONFIG_CONTROL_APN is empty; modem PPP profile is not configured");
	}

	net_mgmt_init_event_callback(&data->netMgmtEventCallback,
					 modem_rc7620_net_mgmt_event_handler,
					 NET_EVENT_PPP_CARRIER_ON |
					 NET_EVENT_PPP_CARRIER_OFF |
					 NET_EVENT_PPP_PHASE_RUNNING |
					 NET_EVENT_PPP_PHASE_DEAD);
	net_mgmt_add_event_callback(&data->netMgmtEventCallback);

	pm_device_init_suspended(dev);
	return 0;
}

struct net_if *modem_rc7620_ppp_iface_get(const struct device *dev)
{
	struct modem_rc7620_data *data;

	if (dev == NULL) {
		return NULL;
	}

	data = dev->data;
	return modem_ppp_get_iface(data->ppp);
}

int modem_rc7620_set_profile(const struct device *dev, const char *apn,
			     const char *id, const char *password)
{
	struct modem_rc7620_data *data;
	int ret;

	if (dev == NULL) {
		return -EINVAL;
	}

	data = dev->data;
	k_mutex_lock(&data->profileLock, K_FOREVER);
	ret = modem_rc7620_set_profile_locked(data, apn, id, password);
	k_mutex_unlock(&data->profileLock);

	return ret;
}

#define MODEM_RC7620_INST_NAME(name, inst) _CONCAT_4(name, _, DT_DRV_COMPAT, inst)

#define MODEM_RC7620_PPP_NET_DEV_NAME(inst) _CONCAT(ppp_net_dev_, MODEM_RC7620_INST_NAME(ppp, inst))
#define MODEM_RC7620_BIND_CONN_EXPANDED(dev_id, conn_id) CONN_MGR_BIND_CONN(dev_id, conn_id)
#define MODEM_RC7620_BIND_CONN(inst, conn_id)                                              \
	MODEM_RC7620_BIND_CONN_EXPANDED(MODEM_RC7620_PPP_NET_DEV_NAME(inst), conn_id)

#define MODEM_RC7620_DEFINE_INSTANCE(inst)                                                       \
	MODEM_PPP_DEFINE(MODEM_RC7620_INST_NAME(ppp, inst), NULL, 98, 1500,                       \
			 CONFIG_MODEM_RC7620_PPP_BUFFER_SIZE);                                     \
	MODEM_RC7620_BIND_CONN(inst, MODEM_LINK_CONN_IMPL);                                  \
	static struct modem_rc7620_data MODEM_RC7620_INST_NAME(data, inst) = {                      \
		.chatDelimiter = (uint8_t *)"\r",                                                 \
		.chatFilter = (uint8_t *)"\n",                                                    \
		.ppp = &MODEM_RC7620_INST_NAME(ppp, inst),                                        \
	};                                                                                         \
	static const struct modem_rc7620_config MODEM_RC7620_INST_NAME(config, inst) = {          \
		.uart = DEVICE_DT_GET(DT_INST_BUS(inst)),                                         \
		.railGpio = GPIO_DT_SPEC_INST_GET(inst, mdm_rail_en_gpios),                       \
		.powerGpio = GPIO_DT_SPEC_INST_GET(inst, mdm_pwr_on_gpios),                       \
		.resetGpio = GPIO_DT_SPEC_INST_GET(inst, mdm_reset_gpios),                        \
		.powerOnPulseMs = CONFIG_MODEM_RC7620_POWER_ON_PULSE_MS,                          \
		.powerOffPulseMs = CONFIG_MODEM_RC7620_POWER_OFF_PULSE_MS,                        \
		.resetPulseMs = CONFIG_MODEM_RC7620_RESET_PULSE_MS,                               \
		.startupMs = CONFIG_MODEM_RC7620_STARTUP_TIME_MS,                                 \
		.shutdownMs = CONFIG_MODEM_RC7620_SHUTDOWN_TIME_MS,                               \
		.autostarts = DT_INST_PROP_OR(inst, auto_start, false),                           \
		.initScript = &rc7620_init_chat_script,                                            \
		.periodicScript = &rc7620_periodic_chat_script,                                    \
		.shutdownScript = NULL,                                                            \
	};                                                                                         \
	PM_DEVICE_DT_INST_DEFINE(inst, modem_rc7620_pm_action);                                   \
	DEVICE_DT_INST_DEFINE(inst, modem_rc7620_init, PM_DEVICE_DT_INST_GET(inst),               \
			      &MODEM_RC7620_INST_NAME(data, inst),                               \
			      &MODEM_RC7620_INST_NAME(config, inst), POST_KERNEL, 99,          \
			      &modem_rc7620_api);

DT_INST_FOREACH_STATUS_OKAY(MODEM_RC7620_DEFINE_INSTANCE)
