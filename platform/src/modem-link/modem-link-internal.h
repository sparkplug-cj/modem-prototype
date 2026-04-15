#pragma once

#include "modem-link.h"

#ifdef __cplusplus
extern "C" {
#endif

enum modem_link_core_event {
	MODEM_LINK_CORE_EVENT_L4_CONNECTED = 0,
	MODEM_LINK_CORE_EVENT_DNS_SERVER_ADD,
};

struct modem_link_core_ops {
	int (*resume_modem)(void *ctx);
	int (*get_ppp_iface)(void *ctx, void **ifaceOut);
	int (*iface_up)(void *ctx, void *iface);
	int (*wait_event)(void *ctx, void *iface, enum modem_link_core_event event,
			  int32_t timeoutMs, uint64_t *raisedEvent);
};

int modem_link_ensure_ready_core(const struct modem_link_core_ops *ops, void *ctx,
				 const struct modem_link_options *options,
				 struct modem_link_diagnostics *diagnostics);

#ifdef __cplusplus
}
#endif
