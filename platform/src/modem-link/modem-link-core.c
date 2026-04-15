#include "modem-link-internal.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

void modem_link_init_diagnostics(struct modem_link_diagnostics *diagnostics)
{
	if (diagnostics == NULL) {
		return;
	}

	memset(diagnostics, 0, sizeof(*diagnostics));
	diagnostics->stage = MODEM_LINK_STAGE_IDLE;
}

struct modem_link_options modem_link_default_options(void)
{
	struct modem_link_options options = {
		.l4TimeoutMs = MODEM_LINK_DEFAULT_L4_TIMEOUT_MS,
		.dnsTimeoutMs = MODEM_LINK_DEFAULT_DNS_TIMEOUT_MS,
		.waitForDns = true,
	};

	return options;
}

const char *modem_link_stage_str(enum modem_link_stage stage)
{
	switch (stage) {
	case MODEM_LINK_STAGE_IDLE:
		return "idle";
	case MODEM_LINK_STAGE_MODEM_RESUMED:
		return "modem-resumed";
	case MODEM_LINK_STAGE_PPP_IFACE_FOUND:
		return "ppp-iface-found";
	case MODEM_LINK_STAGE_PPP_IFACE_UP:
		return "ppp-iface-up";
	case MODEM_LINK_STAGE_L4_CONNECTED:
		return "l4-connected";
	case MODEM_LINK_STAGE_DNS_READY:
		return "dns-ready";
	default:
		return "unknown";
	}
}

static int modem_link_fail(struct modem_link_diagnostics *diagnostics, int ret)
{
	if (diagnostics != NULL) {
		diagnostics->lastError = ret;
	}

	return ret;
}

static bool modem_link_is_nonfatal_ready_state(int ret)
{
	return ret == -EALREADY;
}

int modem_link_ensure_ready_core(const struct modem_link_core_ops *ops, void *ctx,
				 const struct modem_link_options *options,
				 struct modem_link_diagnostics *diagnostics)
{
	struct modem_link_options effectiveOptions;
	void *iface = NULL;
	uint64_t raisedEvent = 0U;
	int ret;

	if ((ops == NULL) || (ops->resume_modem == NULL) || (ops->get_ppp_iface == NULL) ||
	    (ops->iface_up == NULL) || (ops->wait_event == NULL)) {
		return -EINVAL;
	}

	modem_link_init_diagnostics(diagnostics);
	effectiveOptions = (options != NULL) ? *options : modem_link_default_options();

	ret = ops->resume_modem(ctx);
	if ((ret != 0) && !modem_link_is_nonfatal_ready_state(ret)) {
		return modem_link_fail(diagnostics, ret);
	}

	if (diagnostics != NULL) {
		diagnostics->modemResumed = true;
		diagnostics->stage = MODEM_LINK_STAGE_MODEM_RESUMED;
	}

	ret = ops->get_ppp_iface(ctx, &iface);
	if (ret != 0) {
		return modem_link_fail(diagnostics, ret);
	}

	if (diagnostics != NULL) {
		diagnostics->pppInterfaceFound = true;
		diagnostics->stage = MODEM_LINK_STAGE_PPP_IFACE_FOUND;
	}

	ret = ops->iface_up(ctx, iface);
	if ((ret != 0) && !modem_link_is_nonfatal_ready_state(ret)) {
		return modem_link_fail(diagnostics, ret);
	}

	if (diagnostics != NULL) {
		diagnostics->pppInterfaceUp = true;
		diagnostics->stage = MODEM_LINK_STAGE_PPP_IFACE_UP;
	}

	raisedEvent = 0U;
	ret = ops->wait_event(ctx, iface, MODEM_LINK_CORE_EVENT_L4_CONNECTED,
			      effectiveOptions.l4TimeoutMs, &raisedEvent);
	if (ret != 0) {
		return modem_link_fail(diagnostics, ret);
	}

	if (diagnostics != NULL) {
		diagnostics->l4Connected = true;
		diagnostics->lastEvent = raisedEvent;
		diagnostics->stage = MODEM_LINK_STAGE_L4_CONNECTED;
	}

	if (!effectiveOptions.waitForDns) {
		return 0;
	}

	raisedEvent = 0U;
	ret = ops->wait_event(ctx, iface, MODEM_LINK_CORE_EVENT_DNS_SERVER_ADD,
			      effectiveOptions.dnsTimeoutMs, &raisedEvent);
	if (ret != 0) {
		return modem_link_fail(diagnostics, ret);
	}

	if (diagnostics != NULL) {
		diagnostics->dnsServerAdded = true;
		diagnostics->lastEvent = raisedEvent;
		diagnostics->stage = MODEM_LINK_STAGE_DNS_READY;
	}

	return 0;
}
