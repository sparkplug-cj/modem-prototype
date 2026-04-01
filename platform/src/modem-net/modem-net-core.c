#include "modem-net-core.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define MODEM_UART_OWNER_NONE 0
#define MODEM_UART_OWNER_AT 1
#define MODEM_UART_OWNER_PASSTHROUGH 2
#define MODEM_UART_OWNER_PPP 3

static const char *modem_net_owner_name(int owner)
{
	switch (owner) {
	case MODEM_UART_OWNER_AT:
		return "at";
	case MODEM_UART_OWNER_PASSTHROUGH:
		return "passthrough";
	case MODEM_UART_OWNER_PPP:
		return "ppp";
	default:
		return "none";
	}
}

int modem_net_cmd_connect_core(const struct modem_net_ops *ops, size_t argc, char **argv)
{
	const char *failedStage = NULL;
	struct modem_net_profile prof = {0};
	int ret;

	if ((ops == NULL) || (ops->owner_get == NULL) || (ops->ensure_powered == NULL) ||
	    (ops->get_profile == NULL) ||
	    (ops->configure_context == NULL) || (ops->open_uart_session == NULL) ||
	    (ops->dial_ppp == NULL) || (ops->attach_ppp == NULL) ||
	    (ops->wait_for_network == NULL) || (ops->close_uart_session == NULL) ||
	    (ops->escape_and_hangup == NULL) || (ops->set_apn == NULL) ||
	    (ops->set_error == NULL) || (ops->clear_error == NULL) ||
	    (ops->print == NULL) || (ops->error == NULL)) {
		return -EINVAL;
	}

	ops->clear_error();

	(void)argv;

	if (argc != 1U) {
		ops->error(ops->ctx, "usage: modem ppp connect");
		ops->set_error(-EINVAL, "PPP connect takes no arguments");
		return -EINVAL;
	}

	ret = ops->get_profile(&prof);
	if (ret != 0) {
		ops->error(ops->ctx,
			   "PPP profile is not configured; set CONTROL_APN and credentials in prj.secrets.conf");
		ops->set_error(ret, "PPP profile not configured");
		return ret;
	}

	if ((prof.apn == NULL) || (prof.apn[0] == '\0')) {
		ops->error(ops->ctx,
			   "PPP profile is not configured; set CONTROL_APN in prj.secrets.conf");
		ops->set_error(-EINVAL, "PPP profile not configured");
		return -EINVAL;
	}

	if (ops->owner_get() != MODEM_UART_OWNER_NONE) {
		ops->error(ops->ctx, "modem UART is busy");
		ops->set_error(-EBUSY, "modem UART busy");
		return -EBUSY;
	}

	ops->set_apn(prof.apn);

	ops->print(ops->ctx, "PPP connect: ensure modem powered");
	ret = ops->ensure_powered(ops->ctx);
	if (ret != 0) {
		failedStage = "ensure_powered";
		goto out_fail;
	}

	ops->print(ops->ctx, "PPP connect: configure PDP/APN context");
	ret = ops->configure_context(ops->ctx, &prof);
	if (ret != 0) {
		failedStage = "configure_context";
		goto out_fail;
	}

	ops->print(ops->ctx, "PPP connect: open UART session");
	ret = ops->open_uart_session();
	if (ret != 0) {
		failedStage = "open_uart_session";
		goto out_fail;
	}

	ops->print(ops->ctx, "PPP connect: dial PPP");
	ret = ops->dial_ppp(ops->ctx);
	if (ret != 0) {
		failedStage = "dial_ppp";
		goto out_close;
	}

	ops->print(ops->ctx, "PPP connect: attach PPP");
	ret = ops->attach_ppp();
	if (ret != 0) {
		failedStage = "attach_ppp";
		goto out_close;
	}

	ops->print(ops->ctx, "PPP connect: wait for network");
	ret = ops->wait_for_network(ops->ctx);
	if (ret != 0) {
		failedStage = "wait_for_network";
		goto out_close;
	}

	ops->print(ops->ctx, "PPP connected");
	return 0;

out_close:
	if ((failedStage != NULL) && (strcmp(failedStage, "wait_for_network") == 0)) {
		ops->print(ops->ctx, "PPP network wait timed out, tearing down session...");
	}
	(void)ops->escape_and_hangup();
	ops->close_uart_session();
out_fail:
	ops->set_error(ret, "connect failed");
	if (failedStage != NULL) {
		ops->error(ops->ctx, "connect failed at %s: %d", failedStage, ret);
	} else {
		ops->error(ops->ctx, "connect failed: %d", ret);
	}
	return ret;
}

int modem_net_cmd_disconnect_core(const struct modem_net_ops *ops, size_t argc, char **argv)
{
	struct modem_net_status status = {0};
	int ret;

	(void)argc;
	(void)argv;

	if ((ops == NULL) || (ops->get_status == NULL) || (ops->print == NULL) ||
	    (ops->escape_and_hangup == NULL) || (ops->close_uart_session == NULL)) {
		return -EINVAL;
	}

	ret = ops->get_status(&status);
	if (ret != 0) {
		return ret;
	}

	if (!status.sessionOpen) {
		ops->print(ops->ctx, "PPP already disconnected");
		return 0;
	}

	ops->print(ops->ctx, "Disconnecting PPP...");
	(void)ops->escape_and_hangup();
	ops->close_uart_session();
	ops->print(ops->ctx, "PPP disconnected");
	return 0;
}

int modem_net_cmd_status_core(const struct modem_net_ops *ops, size_t argc, char **argv)
{
	struct modem_net_status status = {0};
	const char *pppState;
	int ret;

	(void)argc;
	(void)argv;

	if ((ops == NULL) || (ops->get_status == NULL) || (ops->print == NULL) ||
	    (ops->error == NULL)) {
		return -EINVAL;
	}

	ret = ops->get_status(&status);
	if (ret != 0) {
		ops->error(ops->ctx, "status read failed: %d", ret);
		return ret;
	}

	if (status.connected) {
		pppState = "connected";
	} else if (status.sessionOpen) {
		pppState = "session-open";
	} else {
		pppState = "down";
	}

	ops->print(ops->ctx,
		   "modem=%s owner=%s ppp=%s ip=%s dns=%s apn=%s last_error=%d%s%s",
		   status.modemPowered ? "on" : "off",
		   modem_net_owner_name(status.uartOwner),
		   pppState,
		   ((status.ipv4 != NULL) && (status.ipv4[0] != '\0')) ? status.ipv4 : "-",
		   status.dnsReady ? "ready" : "pending",
		   ((status.apn != NULL) && (status.apn[0] != '\0')) ? status.apn : "-",
		   status.lastError,
		   ((status.lastErrorText != NULL) && (status.lastErrorText[0] != '\0')) ? " " : "",
		   ((status.lastErrorText != NULL) && (status.lastErrorText[0] != '\0')) ? status.lastErrorText : "");
	return 0;
}
