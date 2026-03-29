#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/ppp.h>

#include <string.h>

LOG_MODULE_REGISTER(control_app, LOG_LEVEL_INF);

static struct net_mgmt_event_callback net_cb_l2;
static struct net_mgmt_event_callback net_cb_l3;
static struct net_mgmt_event_callback net_cb_l4;

static bool tcp_test_done = false;
static bool ppp_test_ready = false;

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

static void test_tcp_socket(void)
{
    int sock;
    char rx_buf[256];
    int ret;

    LOG_INF("Starting TCP socket test...");

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    struct addrinfo *res;

    /* DNS retry loop */
    for (int i = 0; i < 5; i++) {
        ret = getaddrinfo("www.fpinfosmart.eu", "80", &hints, &res);
        if (ret == 0) {
            /* Success */
            break;
        }

        LOG_WRN("DNS retry %d failed: %d", i + 1, ret);
        k_sleep(K_MSEC(500));
    }

    /* If still failed after all retries → return */
    if (ret != 0) {
        LOG_ERR("DNS lookup failed after retries, ret=%d", ret);
        return;
    }

    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        LOG_ERR("socket() failed");
        freeaddrinfo(res);
        return;
    }

    ret = connect(sock, res->ai_addr, res->ai_addrlen);
    if (ret < 0) {
        LOG_ERR("connect() failed");
        close(sock);
        freeaddrinfo(res);
        return;
    }

    freeaddrinfo(res);

    LOG_INF("TCP connected");

    const char *http_req =
        "GET /ModemWS/GMWS.asmx HTTP/1.1\r\n"
        "Host: www.fpinfosmart.eu\r\n"
        "Connection: close\r\n"
        "\r\n";

    send(sock, http_req, strlen(http_req), 0);

    while ((ret = recv(sock, rx_buf, sizeof(rx_buf) - 1, 0)) > 0) {
        rx_buf[ret] = '\0';
        LOG_INF("RX:\n%s", rx_buf);
    }

    close(sock);

    ppp_test_ready = false;

    LOG_INF("TCP socket test done");
}

// net_mgmt event handler
static void net_event_handler(struct net_mgmt_event_callback *cb,
                              uint64_t mgmt_event,
                              struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if ((mgmt_event == NET_EVENT_PPP_PHASE_RUNNING ||
         mgmt_event == NET_EVENT_IPV4_ADDR_ADD ||
         mgmt_event == NET_EVENT_L4_CONNECTED ) && !tcp_test_done) 
    {

        tcp_test_done = true;
        ppp_test_ready = true;

        LOG_INF("PPP is RUNNING → starting TCP test");
    }
}

int main(void)
{
    LOG_INF("control app boot");

    tcp_test_done = false;
    ppp_test_ready = false;

    /* net_mgmt : register event handler */
    // 1. L2 (PPP) 
    net_mgmt_init_event_callback(&net_cb_l2,
                                 net_event_handler,
                                 NET_EVENT_PPP_PHASE_RUNNING );
    net_mgmt_add_event_callback(&net_cb_l2);

    // 2. L3 (IPv4 / DNS)
    net_mgmt_init_event_callback(&net_cb_l3,
                                 net_event_handler,
                                 NET_EVENT_IPV4_ADDR_ADD |
                                 NET_EVENT_DNS_SERVER_ADD);
    net_mgmt_add_event_callback(&net_cb_l3);

        // 3. L4 (Socket)
    net_mgmt_init_event_callback(&net_cb_l4,
                                 net_event_handler,
                                 NET_EVENT_L4_CONNECTED );
    net_mgmt_add_event_callback(&net_cb_l4);

    while (1) {

        if(ppp_test_ready)
        {
            test_tcp_socket();
        }
        k_sleep(K_SECONDS(1));
    }

    return 0;
}
