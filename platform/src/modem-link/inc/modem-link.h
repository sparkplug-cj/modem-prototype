#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct net_if;

#define MODEM_LINK_TIMEOUT_FOREVER (-1)
#define MODEM_LINK_DEFAULT_L4_TIMEOUT_MS 120000
#define MODEM_LINK_DEFAULT_DNS_TIMEOUT_MS 10000

enum modem_link_stage {
	MODEM_LINK_STAGE_IDLE = 0,
	MODEM_LINK_STAGE_MODEM_RESUMED,
	MODEM_LINK_STAGE_PPP_IFACE_FOUND,
	MODEM_LINK_STAGE_PPP_IFACE_UP,
	MODEM_LINK_STAGE_L4_CONNECTED,
	MODEM_LINK_STAGE_DNS_READY,
};

struct modem_link_options {
	int32_t l4TimeoutMs;
	int32_t dnsTimeoutMs;
	bool waitForDns;
};

struct modem_link_diagnostics {
	enum modem_link_stage stage;
	int lastError;
	uint64_t lastEvent;
	bool modemResumed;
	bool pppInterfaceFound;
	bool pppInterfaceUp;
	bool l4Connected;
	bool dnsServerAdded;
};

void modem_link_init_diagnostics(struct modem_link_diagnostics *diagnostics);
struct modem_link_options modem_link_default_options(void);
const char *modem_link_stage_str(enum modem_link_stage stage);
int modem_link_get_ppp_iface(struct net_if **out);
int modem_link_ensure_ready(const struct modem_link_options *options,
				    struct modem_link_diagnostics *diagnostics);

#ifdef __cplusplus
}
#endif
