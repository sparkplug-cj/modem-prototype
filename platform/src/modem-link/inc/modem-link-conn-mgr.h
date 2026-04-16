#pragma once

#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/net/conn_mgr_connectivity_impl.h>

#ifdef __cplusplus
extern "C" {
#endif

struct modem_link_conn_mgr_ctx {
	struct k_work connectWork;
	struct k_work disconnectWork;
	struct conn_mgr_conn_binding *binding;
	bool connectRequested;
	bool disconnectRequested;
};

#define MODEM_LINK_CONN_IMPL_CTX_TYPE struct modem_link_conn_mgr_ctx

CONN_MGR_CONN_DECLARE_PUBLIC(MODEM_LINK_CONN_IMPL);

#ifdef __cplusplus
}
#endif
