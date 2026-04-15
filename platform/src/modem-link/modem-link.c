#include "modem-link-internal.h"

#include "modem-board.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/ppp.h>
#include <zephyr/pm/device.h>

LOG_MODULE_REGISTER(modem_link, CONFIG_LOG_DEFAULT_LEVEL);

#if DT_HAS_ALIAS(modem)
static const struct device *const modemDevice = DEVICE_DT_GET(DT_ALIAS(modem));
#else
static const struct device *const modemDevice;
#endif

static bool modem_link_board_status_indicates_on(const struct modem_board_status *status)
{
	return (status->rail_en == 1) || status->modem_state_on;
}

static int modem_link_power_on_board_if_needed(void)
{
	struct modem_board_status status;
	int ret;

	ret = modem_board_get_status(&status);
	if (ret != 0) {
		return ret;
	}

	if (modem_link_board_status_indicates_on(&status)) {
		return 0;
	}

	return modem_board_power_on();
}

static void modem_link_power_off_board_if_needed(void)
{
	struct modem_board_status status;

	if (modem_board_get_status(&status) != 0) {
		return;
	}

	if (!modem_link_board_status_indicates_on(&status)) {
		return;
	}

	(void)modem_board_power_off();
}

static int modem_link_suspend_modem_zephyr(void)
{
#if !DT_HAS_ALIAS(modem)
	return -ENODEV;
#else
	if (!device_is_ready(modemDevice)) {
		return -ENODEV;
	}

#if defined(CONFIG_PM_DEVICE)
	return pm_device_action_run(modemDevice, PM_DEVICE_ACTION_SUSPEND);
#else
	return 0;
#endif
#endif
}

static int modem_link_resume_modem_zephyr(void *ctx)
{
	int ret;

	(void)ctx;

#if !DT_HAS_ALIAS(modem)
	return -ENODEV;
#else
	ret = modem_link_power_on_board_if_needed();
	if (ret != 0) {
		return ret;
	}

	if (!device_is_ready(modemDevice)) {
		return -ENODEV;
	}

#if defined(CONFIG_PM_DEVICE)
	return pm_device_action_run(modemDevice, PM_DEVICE_ACTION_RESUME);
#else
	return 0;
#endif
#endif
}

static int modem_link_get_ppp_iface_zephyr(void *ctx, void **ifaceOut)
{
	struct net_if *iface;

	(void)ctx;

	if (ifaceOut == NULL) {
		return -EINVAL;
	}

	iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
	if (iface == NULL) {
		return -ENODEV;
	}

	*ifaceOut = iface;
	return 0;
}

static int modem_link_iface_up_zephyr(void *ctx, void *iface)
{
	(void)ctx;

	if (iface == NULL) {
		return -EINVAL;
	}

	return net_if_up((struct net_if *)iface);
}

static k_timeout_t modem_link_timeout_from_ms(int32_t timeoutMs)
{
	if (timeoutMs < 0) {
		return K_FOREVER;
	}

	return K_MSEC(timeoutMs);
}

static uint64_t modem_link_event_mask(enum modem_link_core_event event)
{
	switch (event) {
	case MODEM_LINK_CORE_EVENT_L4_CONNECTED:
		return NET_EVENT_L4_CONNECTED;
	case MODEM_LINK_CORE_EVENT_DNS_SERVER_ADD:
		return NET_EVENT_DNS_SERVER_ADD;
	default:
		return 0U;
	}
}

static int modem_link_wait_event_zephyr(void *ctx, void *iface,
				 enum modem_link_core_event event,
				 int32_t timeoutMs, uint64_t *raisedEvent)
{
	uint64_t eventMask;

	(void)ctx;

	if (iface == NULL) {
		return -EINVAL;
	}

	eventMask = modem_link_event_mask(event);
	if (eventMask == 0U) {
		return -EINVAL;
	}

	return net_mgmt_event_wait_on_iface((struct net_if *)iface,
					    eventMask,
					    raisedEvent,
					    NULL,
					    NULL,
					    modem_link_timeout_from_ms(timeoutMs));
}

static const struct modem_link_core_ops modemLinkCoreOps = {
	.resume_modem = modem_link_resume_modem_zephyr,
	.get_ppp_iface = modem_link_get_ppp_iface_zephyr,
	.iface_up = modem_link_iface_up_zephyr,
	.wait_event = modem_link_wait_event_zephyr,
};

static bool modem_link_should_preserve_runtime_state(const struct modem_link_diagnostics *diagnostics)
{
	if (diagnostics == NULL) {
		return false;
	}

	return diagnostics->l4Connected;
}

static const char *modem_link_diagnostics_stage_or_unknown(
	const struct modem_link_diagnostics *diagnostics)
{
	if (diagnostics == NULL) {
		return "<none>";
	}

	return modem_link_stage_str(diagnostics->stage);
}

int modem_link_get_ppp_iface(struct net_if **out)
{
	void *iface = NULL;
	int ret;

	if (out == NULL) {
		return -EINVAL;
	}

	ret = modem_link_get_ppp_iface_zephyr(NULL, &iface);
	if (ret != 0) {
		*out = NULL;
		return ret;
	}

	*out = (struct net_if *)iface;
	return 0;
}

int modem_link_ensure_ready(const struct modem_link_options *options,
				    struct modem_link_diagnostics *diagnostics)
{
	int ret = modem_link_ensure_ready_core(&modemLinkCoreOps, NULL, options, diagnostics);

	if (ret == 0) {
		LOG_INF("PPP ready core completed stage=%s",
			modem_link_diagnostics_stage_or_unknown(diagnostics));
		return 0;
	}

	if (modem_link_should_preserve_runtime_state(diagnostics)) {
		LOG_INF("PPP ready failure preserved runtime ret=%d stage=%s l4=%d dns=%d",
			ret,
			modem_link_diagnostics_stage_or_unknown(diagnostics),
			diagnostics->l4Connected ? 1 : 0,
			diagnostics->dnsServerAdded ? 1 : 0);
		return ret;
	}

	LOG_WRN("PPP ready failure tearing modem down ret=%d stage=%s l4=%d dns=%d",
		ret,
		modem_link_diagnostics_stage_or_unknown(diagnostics),
		diagnostics->l4Connected ? 1 : 0,
		diagnostics->dnsServerAdded ? 1 : 0);

	(void)modem_link_suspend_modem_zephyr();
	modem_link_power_off_board_if_needed();
	return ret;
}
