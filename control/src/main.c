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

#define CONTROL_TLS_SEC_TAG 1

static const unsigned char ca_cert_pem[]={
    #include "ca_cert_pem.inc"
    0x00, 0x00    
};
static const size_t ca_cert_pem_len = sizeof(ca_cert_pem) - 1;

static int NormalizePemBlock(const char *label,
                             const char *rawPem,
                             char *normalizedPem,
                             size_t normalizedPemSize,
                             const char *beginMarker,
                             const char *endMarker,
                             size_t *normalizedLen)
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

    ret = NormalizePemCertificate(rawPem,
                                  normalizedPem,
                                  normalizedPemSize,
                                  normalizedLen,
                                  &newlineCount);
    if (ret < 0) {
        LOG_ERR("%s normalization failed: %d (raw_len=%u)",
                label,
                ret,
                (unsigned int)rawLen);
        return ret;
    }

    if (!PemBlockLooksValid(normalizedPem, beginMarker, endMarker)) {
        LOG_ERR("%s missing PEM markers", label);
        return -EINVAL;
    }

    if (newlineCount < 2U) {
        LOG_ERR("%s looks malformed after normalization: newline_count=%u raw_len=%u norm_len=%u",
                label,
                (unsigned int)newlineCount,
                (unsigned int)rawLen,
                (unsigned int)*normalizedLen);
        return -EINVAL;
    }

    /* Certificate configuration */
    ret = tls_credential_add(CONTROL_TLS_SEC_TAG,
                             TLS_CREDENTIAL_CA_CERTIFICATE,
                             ca_cert_pem,
                             ca_cert_pem_len);
    if (ret < 0) {
        return ret;
    }

    ret = tls_credential_add(secTag,
                             credentialType,
                             normalizedPem,
                             normalizedLen + 1U);
    if (ret < 0) {
        LOG_ERR("%s registration failed: %d", label, ret);
        return ret;
    }

    LOG_INF("%s registered (sec_tag=%d len=%u)",
            label,
            secTag,
            (unsigned int)normalizedLen);
    return 0;
}

static int SendHttpChunkedRequest(int sock,
                                  const char *serverHost,
                                  const char *serverUrl,
                                  const char *requestBody)
{
    char requestHeaders[384];
    char chunkHeader[16];
    size_t requestBodyLen = strlen(requestBody);
    int ret;

    ret = snprintf(requestHeaders, sizeof(requestHeaders),
                   "POST %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "Accept: application/octet-stream\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Transfer-Encoding: chunked\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   serverUrl,
                   serverHost);
    if ((ret < 0) || (ret >= (int)sizeof(requestHeaders))) {
        LOG_ERR("HTTP header buffer too small");
        return -ENOMEM;
    }

    ret = send(sock, requestHeaders, ret, 0);
    if (ret < 0) {
        LogErrnoDetail("send(headers)");
        return ret;
    }
    LOG_INF("HTTP headers sent (%d bytes)", ret);

    ret = snprintf(chunkHeader, sizeof(chunkHeader), "%zx\r\n", requestBodyLen);
    if ((ret < 0) || (ret >= (int)sizeof(chunkHeader))) {
        LOG_ERR("HTTP chunk header buffer too small");
        return -ENOMEM;
    }

    ret = send(sock, chunkHeader, ret, 0);
    if (ret < 0) {
        LogErrnoDetail("send(chunk header)");
        return ret;
    }

    ret = send(sock, requestBody, requestBodyLen, 0);
    if (ret < 0) {
        LogErrnoDetail("send(chunk body)");
        return ret;
    }
    LOG_INF("HTTP chunk body sent (%u bytes)", (unsigned int)requestBodyLen);

    ret = send(sock, "\r\n0\r\n\r\n", 7, 0);
    if (ret < 0) {
        LogErrnoDetail("send(chunk terminator)");
        return ret;
    }
    LOG_INF("HTTP chunk terminator sent");
    return 0;
}

static int tls_setup(void)
{
    const char *rawServerCaCert = (const char *)server_ca_cert_pem;
    const char *rawMtlClientCert = (const char *)mtls_client_cert_pem;
    const char *rawMtlsPrivateKey = (const char *)mtls_private_key_pem;
    int ret;

    ret = RegisterPemCredential(CONTROL_TLS_SEC_TAG,
                                TLS_CREDENTIAL_CA_CERTIFICATE,
                                "Server CA certificate",
                                rawServerCaCert,
                                server_ca_cert_pem_normalized,
                                sizeof(server_ca_cert_pem_normalized),
                                "-----BEGIN CERTIFICATE-----",
                                "-----END CERTIFICATE-----");
    if (ret < 0) {
        return ret;
    }

    if ((strlen(rawMtlClientCert) == 0U) && (strlen(rawMtlsPrivateKey) == 0U)) {
        LOG_INF("mTLS client credentials not configured");
        return 0;
    }

    if ((strlen(rawMtlClientCert) == 0U) || (strlen(rawMtlsPrivateKey) == 0U)) {
        LOG_ERR("mTLS requires both client certificate and private key");
        return -EINVAL;
    }

    ret = RegisterPemCredential(CONTROL_TLS_SEC_TAG,
                                TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
                                "mTLS client certificate",
                                rawMtlClientCert,
                                mtls_client_cert_pem_normalized,
                                sizeof(mtls_client_cert_pem_normalized),
                                "-----BEGIN CERTIFICATE-----",
                                "-----END CERTIFICATE-----");
    if (ret < 0) {
        return ret;
    }

    ret = RegisterPemCredential(CONTROL_TLS_SEC_TAG,
                                TLS_CREDENTIAL_PRIVATE_KEY,
                                "mTLS private key",
                                rawMtlsPrivateKey,
                                mtls_private_key_pem_normalized,
                                sizeof(mtls_private_key_pem_normalized),
                                "-----BEGIN EC PRIVATE KEY-----",
                                "-----END EC PRIVATE KEY-----");
    if (ret < 0) {
        return ret;
    }

    return 0;
}



static void test_tcp_socket(void)
{
    int sock;
    char port[6];
    char rx_buf[256];
    const char *serverHost = CONFIG_CONTROL_SERVER_HOST;
    const char *serverUrl = CONFIG_CONTROL_SERVER_URL;
    const char *request_body = "Hello word";
    int ret;
    int64_t connectStartMs;
    int64_t connectElapsedMs;
    int statusCode = -1;
    static bool tls_credential_added = false;

    LOG_INF("Starting TCP socket test...");
    LOG_INF("HTTP target: host=%s port=%d url=%s", serverHost, CONFIG_CONTROL_SERVER_PORT, serverUrl);

    if(!tls_credential_added)
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
        (strlen(serverUrl) == 0U)) {
        LOG_ERR("Server host or URL is not configured");
        return;
    }

    snprintf(port, sizeof(port), "%d", CONFIG_CONTROL_SERVER_PORT);

    /* DNS */
    ret = getaddrinfo(serverHost, port, &hints, &res);
    if (ret != 0) {
        LOG_ERR("DNS lookup failed: %d", ret);
        return;
    }
    LOG_INF("DNS resolved: family=%d socktype=%d proto=%d", res->ai_family, res->ai_socktype,
            res->ai_protocol);

    // sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    sock = socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
    if (sock < 0) {
        LogErrnoDetail("socket(IPPROTO_TLS_1_2)");
        freeaddrinfo(res);
        return;
    }
    LOG_INF("TLS socket created: fd=%d", sock);

    // tls configuration
    {
        sec_tag_t sec_tag_list[] = { CONTROL_TLS_SEC_TAG };

        ret = setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
                         sec_tag_list, sizeof(sec_tag_list));
        if (ret < 0) {
            LogErrnoDetail("setsockopt(TLS_SEC_TAG_LIST)");
            close(sock);
            freeaddrinfo(res);
            return;
        }
        LOG_INF("TLS sec tag configured: %d", CONTROL_TLS_SEC_TAG);

        ret = setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
                         serverHost,
                         strlen(serverHost));
        if (ret < 0) {
            LogErrnoDetail("setsockopt(TLS_HOSTNAME)");
            close(sock);
            freeaddrinfo(res);
            return;
        }
        LOG_INF("TLS hostname set: %s", serverHost);

        int verify = TLS_PEER_VERIFY_REQUIRED;
        ret = setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
                         &verify, sizeof(verify));
        if (ret < 0) {
            LogErrnoDetail("setsockopt(TLS_PEER_VERIFY)");
            close(sock);
            freeaddrinfo(res);
            return;
        }
        LOG_INF("TLS peer verification required");
    }

    /* recv timeout */
    {
        struct timeval tv = {
            .tv_sec = 10,
            .tv_usec = 0,
        };
        ret = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (ret < 0) {
            LogErrnoDetail("setsockopt(SO_RCVTIMEO)");
            close(sock);
            freeaddrinfo(res);
            return;
        }
    }

    LOG_INF("Starting connect (TCP + TLS handshake)...");
    connectStartMs = k_uptime_get();
    ret = connect(sock, res->ai_addr, res->ai_addrlen);
    connectElapsedMs = k_uptime_get() - connectStartMs;
    freeaddrinfo(res);

    if (ret < 0) {
        LOG_ERR("connect()/handshake failed after %lld ms", (long long)connectElapsedMs);
        LogErrnoDetail("connect");
        close(sock);
        return;
    }

    LOG_INF("TCP/TLS connected in %lld ms", (long long)connectElapsedMs);

    ret = snprintf(http_req, sizeof(http_req),
                   "POST %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "User-Agent: ZephyrTLS/1.0\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Content-Length: %zu\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "%s",
                   serverUrl,
                   serverHost,
                   request_body_len,
                   request_body);
    if ((ret < 0) || (ret >= (int)sizeof(http_req))) {
        LOG_ERR("HTTP request buffer too small");
        close(sock);
        return;
    }

    ret = send(sock, http_req, ret, 0);
    if (ret < 0) {
        close(sock);
        return;
    }

    ret = recv(sock, rx_buf, sizeof(rx_buf) - 1, 0);
    if (ret > 0) {
        rx_buf[ret] = '\0';
        if (sscanf(rx_buf, "HTTP/%*u.%*u %d", &statusCode) == 1) {
            LOG_INF("HTTP status code: %d", statusCode);
        } else {
            LOG_WRN("Could not parse HTTP status line");
        }
        LOG_INF("RX first chunk (%d bytes):\n%s", ret, rx_buf);
    } else if (ret == 0) {
        LOG_WRN("recv(): connection closed by peer");
    } else {
        LogErrnoDetail("recv");
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
