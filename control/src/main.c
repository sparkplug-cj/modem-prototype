#include "modem-link.h"

#include <inttypes.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>

LOG_MODULE_REGISTER(control_app, LOG_LEVEL_INF);

static void control_app_log_ipv4_address(struct net_if *iface)
{
    const struct in_addr *address;
    char buffer[NET_IPV4_ADDR_LEN];

    address = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (address == NULL) {
        LOG_WRN("No IPv4 address assigned yet");
        return;
    }

    if (net_addr_ntop(AF_INET, address, buffer, sizeof(buffer)) == NULL) {
        LOG_WRN("Failed to format IPv4 address");
        return;
    }

    LOG_INF("PPP IPv4 address: %s", buffer);
}

int main(void)
{
    struct net_if *pppIface = NULL;
    uint64_t raisedEvent = 0U;
    int ret;

    LOG_INF("Control app boot: connecting through conn_mgr");

    ret = modem_link_get_ppp_iface(&pppIface);
    if ((ret != 0) || (pppIface == NULL)) {
        LOG_ERR("PPP iface lookup failed: %d", ret);
        return 0;
    }

    ret = conn_mgr_if_set_timeout(pppIface, MODEM_LINK_DEFAULT_L4_TIMEOUT_MS / MSEC_PER_SEC);
    if (ret != 0) {
        LOG_WRN("conn_mgr_if_set_timeout failed: %d", ret);
    }

    ret = conn_mgr_if_connect(pppIface);
    if (ret != 0) {
        LOG_ERR("conn_mgr_if_connect failed: %d", ret);
        return 0;
    }

    LOG_INF("Waiting for NET_EVENT_L4_CONNECTED...");
    ret = net_mgmt_event_wait_on_iface(pppIface,
                                                                         NET_EVENT_L4_CONNECTED,
                                                                         &raisedEvent,
                                                                         NULL,
                                                                         NULL,
                                                                         K_MSEC(MODEM_LINK_DEFAULT_L4_TIMEOUT_MS));
    if (ret != 0) {
        LOG_ERR("Timed out waiting for PPP L4 connectivity: %d", ret);
        return 0;
    }

    LOG_INF("PPP connected, event=0x%" PRIx64, raisedEvent);

    control_app_log_ipv4_address(pppIface);

    ret = conn_mgr_if_disconnect(pppIface);
    if (ret != 0) {
        LOG_ERR("conn_mgr_if_disconnect failed: %d", ret);
        return 0;
    }
    
    while (1) {
        k_sleep(K_FOREVER);
    }
}
