#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/ppp.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include "modem-net.h"
#include "modem-board.h"


LOG_MODULE_REGISTER(control_app, LOG_LEVEL_INF);

static struct net_mgmt_event_callback net_cb_l2;
static struct net_mgmt_event_callback net_cb_l3;
static struct net_mgmt_event_callback net_cb_l4;

static bool tcp_test_done = false;
static bool ppp_test_ready = false;

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <zephyr/net/tls_credentials.h>

#include "one_month_arr.h"

#define CONTROL_TLS_SEC_TAG 1

static const unsigned char ca_cert_pem[]={
    #include "ca_cert_pem.inc"
    0x00, 0x00    
};
static const size_t ca_cert_pem_len = sizeof(ca_cert_pem) - 1;

static int tls_setup(void)
{
    LOG_INF("CERTI STRING : strlen=%d, sizeof=%d\n", strlen(ca_cert_pem), sizeof(ca_cert_pem));
    LOG_INF("=== PEM START ===\n%s\n=== PEM END ===\n", ca_cert_pem);
    
    // Check certi string
    for (int i = 0; i < 20; i++) {
        LOG_INF("%02X ", ca_cert_pem[i]);
    }

    if (strlen(ca_cert_pem) == 0U) {
        LOG_ERR("CONFIG_CONTROL_TLS_CA_CERT_PEM is empty");
        return -1;
    }

    /* Certificate configuration */
    int ret = tls_credential_add(CONTROL_TLS_SEC_TAG,
                             TLS_CREDENTIAL_CA_CERTIFICATE,
                             ca_cert_pem,
                             ca_cert_pem_len);
    if (ret < 0) {
        LOG_ERR("tls_credential_add failed: %d", ret);
        return ret;
    }
    LOG_INF("TLS CA certificate registered (sec_tag=%d)", CONTROL_TLS_SEC_TAG);
    return 0;
}


static void test_tcp_socket(void)
{
    int sock;
    char http_req[256];
    char port[6];
    char rx_buf[256];
    const char *serverHost = CONFIG_CONTROL_SERVER_HOST; 
    const char *serverUrl  = CONFIG_CONTROL_SERVER_URL;
    int ret;
    static bool tls_credential_added = false;

    LOG_INF("Starting TCP socket test...");

    if (!tls_credential_added)
    {
        if (tls_setup() < 0)
        {
            return;
        }
        tls_credential_added = true;
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;

    if ((strlen(serverHost) == 0U) ||
        (strlen(serverUrl)  == 0U))
    {
        LOG_ERR("Server host or URL is not configured");
        return;
    }

    snprintf(port, sizeof(port), "%d", CONFIG_CONTROL_SERVER_PORT);

    /* DNS */
    ret = getaddrinfo(serverHost, port, &hints, &res);
    if (ret != 0)
    {
        LOG_ERR("DNS lookup failed: %d", ret);
        return;
    }

    /* TLS socket */
    sock = socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
    if (sock < 0)
    {
        LOG_ERR("socket() failed - errno = %d", errno);
        freeaddrinfo(res);
        return;
    }

    /* TLS configuration */
    {
        sec_tag_t sec_tag_list[] = { CONTROL_TLS_SEC_TAG };

        setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
                    sec_tag_list, sizeof(sec_tag_list));

        setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
                    serverHost, strlen(serverHost));

        int verify = TLS_PEER_VERIFY_REQUIRED;
        setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
                    &verify, sizeof(verify));
    }

    /* recv timeout */
    {
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* CONNECT */
    ret = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (ret < 0)
    {
        LOG_ERR("connect() failed - errno = %d", errno);
        close(sock);
        return;
    }

    LOG_INF("TCP/TLS connected");

    // send header
    // ret = snprintf(http_req, sizeof(http_req),
    //                "POST %s HTTP/1.1\r\n"
    //                "Host: %s\r\n"
    //                "User-Agent: ZephyrTLS/1.0\r\n"
    //                "Content-Type: text/plain\r\n"
    //                "Content-Length: %u\r\n"
    //                "Connection: close\r\n"
    //                "\r\n",
    //                serverUrl,
    //                serverHost,
    //                one_month_txt_len);
    ret = snprintf(http_req, sizeof(http_req),
                   "POST %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "Accept: application/octet-stream\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Content-Length: %u\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   serverUrl,
                   serverHost,
                   one_month_txt_len);

    if (ret < 0 || ret >= (int)sizeof(http_req))
    {
        LOG_ERR("HTTP request buffer too small");
        close(sock);
        return;
    }

    LOG_INF("Sending header (%d bytes)", ret);

    ret = send(sock, http_req, ret, 0);
    if (ret < 0)
    {
        LOG_ERR("send(header) failed - errno = %d", errno);
        close(sock);
        return;
    }

    // send test data
    LOG_INF("Uploading file (%u bytes)...", one_month_txt_len);

    size_t offset = 0;
    const size_t CHUNK = 1024;


    size_t total_sent = 0;

    while (offset < one_month_txt_len)
    {
        size_t len = one_month_txt_len - offset;
        if (len > CHUNK)
            len = CHUNK;

        ret = send(sock, &one_month_txt[offset], len, 0);
        LOG_INF("============== SENT : %d ============================", ret);
        if (ret < 0) {
            LOG_ERR("send() failed at offset %u, errno=%d", offset, errno);
            break;
        }

        if (ret == 0) {
            LOG_ERR("send() returned 0 at offset %u (connection closed?)", offset);
            break;
        }
        
        total_sent += ret;
        offset += ret; 

        LOG_INF("Sent chunk: %d bytes (total: %u)", ret, total_sent);
    }

    LOG_INF("Upload finished. Actually sent: %u bytes", total_sent);

    k_sleep(K_MSEC(100));

    // receive response from server
    ret = recv(sock, rx_buf, sizeof(rx_buf) - 1, 0);
    if (ret > 0)
    {
        rx_buf[ret] = '\0';
        LOG_INF("RX (%d bytes):\n%s", ret, rx_buf);
    }
    else if (ret == 0)
    {
        LOG_WRN("recv(): connection closed by peer");
    }
    else
    {
        LOG_ERR("recv() error: %d", errno);
    }

    close(sock);
    ppp_test_ready = false;

    LOG_INF("TCP upload test done");
}

// net_mgmt event handler
static void net_event_handler(struct net_mgmt_event_callback *cb,
                              uint64_t mgmt_event,
                              struct net_if *iface)
{
    ARG_UNUSED(cb);

    if ((mgmt_event == NET_EVENT_IPV4_ADDR_ADD ||
         mgmt_event == NET_EVENT_L4_CONNECTED) && !tcp_test_done) 
    {
        const char *trigger =
            (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) ? "IPv4 address assigned" :
            (mgmt_event == NET_EVENT_L4_CONNECTED) ? "L4 connected" :
            "unknown";

        tcp_test_done = true;
        ppp_test_ready = true;

        net_if_set_default(iface);

        LOG_INF("%s -> starting TCP test", trigger);
    }
}


static void dump_iface(struct net_if *iface, void *user_data)
{
    ARG_UNUSED(user_data);

    char buf[NET_IPV4_ADDR_LEN];

    printk("iface: %s\n", net_if_get_device(iface)->name);

    /* IPv4 address */
    const struct in_addr *ip =
        net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

    if (ip) {
        net_addr_ntop(AF_INET, ip, buf, sizeof(buf));
        printk("  IP      : %s\n", buf);
    } else {
        printk("  IP      : (none)\n");
    }

    /* Gateway */
    struct in_addr gw = net_if_ipv4_get_gw(iface);

    if (gw.s_addr != 0) {
        net_addr_ntop(AF_INET, &gw, buf, sizeof(buf));
        printk("  Gateway : %s\n", buf);
    } else {
        printk("  Gateway : (none)\n");
    }

    printk("\n");
}

static void dump_ipv4_ifaces(void)
{
    printk("=== IPv4 iface info ===\n");
    net_if_foreach(dump_iface, NULL);
    printk("=======================\n");
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


    {

        int ret = modem_board_power_on();
        if (ret) {
            LOG_ERR("MODEM power on failed: %d", ret);
            return 0;
        }

        struct net_if *iface = modem_net_ppp_iface_get();

        if (!iface) {
            LOG_ERR("PPP iface not available");
            return 0;
        }

        struct modem_net_ppp_profile profile = {
            .apn = CONFIG_CONTROL_APN,
            .id = CONFIG_CONTROL_APN_USERNAME,
            .password = CONFIG_CONTROL_APN_PASSWORD,
        };

        int status = conn_mgr_if_set_opt(iface, MODEM_NET_PPP_OPT_PROFILE, &profile, sizeof(profile));
        if (status) {
            LOG_ERR("Failed to set PPP profile: %d", status);
            return 0;
        }

        status = conn_mgr_if_connect(iface);
        if (status) {
            LOG_ERR("Failed to connect PPP iface: %d", status);
            return 0;
        }
        
    }
    
    while (1) {

        if(ppp_test_ready)
        {

            dump_ipv4_ifaces();
            
            struct net_if *iface = net_if_get_default();
            
            if (!iface) {
                LOG_ERR("No default net_if");
            }


            if (net_if_is_up(iface)) {
                LOG_INF("net_if is UP");
            } else {
                LOG_INF("net_if is DOWN");
            }


            const struct in_addr *addr;

            addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

            if (addr) {
                char buf[NET_IPV4_ADDR_LEN];

                net_addr_ntop(AF_INET, addr, buf, sizeof(buf));
                LOG_INF("IPv4 address: %s", buf);
            } else {
                LOG_INF("No IPv4 address yet");
            }


            test_tcp_socket();
        }
        k_sleep(K_SECONDS(1));
    }

    return 0;
}
