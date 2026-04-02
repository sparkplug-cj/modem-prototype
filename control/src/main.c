#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/ppp.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "one_month.h"

LOG_MODULE_REGISTER(control_app, LOG_LEVEL_INF);

static struct net_mgmt_event_callback net_cb_l2;
static struct net_mgmt_event_callback net_cb_l3;
static struct net_mgmt_event_callback net_cb_l4;

static bool tcp_test_done = false;
static bool ppp_test_ready = false;

#define CONTROL_TLS_SEC_TAG 1
#define CONTROL_HTTP_TIMEOUT_MS (180 * MSEC_PER_SEC)
#define CONTROL_HTTP_CHUNK_SIZE 1024U

struct upload_progress {
    size_t total_size;
    size_t bytes_sent;
    size_t chunks_sent;
};

struct upload_context {
    struct upload_progress progress;
    unsigned int http_status_code;
    bool http_status_logged;
    bool response_complete;
};

static size_t normalize_pem_string(const char *src, char *dst, size_t dst_size)
{
    size_t src_idx = 0U;
    size_t dst_idx = 0U;

    while ((src[src_idx] != '\0') && (dst_idx + 1U < dst_size)) {
        if (src[src_idx] == '\\') {
            /* Handle both escaped (\\n) and double-escaped (\\\\n) newline forms. */
            if ((src[src_idx + 1U] == '\\') && (src[src_idx + 2U] != '\0')) {
                switch (src[src_idx + 2U]) {
                case 'n':
                    dst[dst_idx++] = '\n';
                    src_idx += 3U;
                    continue;
                case 'r':
                    dst[dst_idx++] = '\r';
                    src_idx += 3U;
                    continue;
                case 't':
                    dst[dst_idx++] = '\t';
                    src_idx += 3U;
                    continue;
                default:
                    break;
                }
            }

            if (src[src_idx + 1U] != '\0') {
                switch (src[src_idx + 1U]) {
                case 'n':
                    dst[dst_idx++] = '\n';
                    src_idx += 2U;
                    continue;
                case 'r':
                    dst[dst_idx++] = '\r';
                    src_idx += 2U;
                    continue;
                case 't':
                    dst[dst_idx++] = '\t';
                    src_idx += 2U;
                    continue;
                case '\\':
                    dst[dst_idx++] = '\\';
                    src_idx += 2U;
                    continue;
                default:
                    break;
                }
            }
        }

        dst[dst_idx++] = src[src_idx++];
    }

    dst[dst_idx] = '\0';
    return dst_idx;
}

static int tls_setup(void)
{
    static char ca_cert_pem[sizeof(CONFIG_CONTROL_TLS_SERVER_CA_CERT_PEM)];
    const char *ca_cert_pem_raw = CONFIG_CONTROL_TLS_SERVER_CA_CERT_PEM;

    if (strlen(ca_cert_pem_raw) == 0U) {
        LOG_ERR("TLS certificate empty - check prj.secrets.conf CONFIG_CONTROL_TLS_SERVER_CA_CERT_PEM");
        return -1;
    }

    const size_t ca_cert_pem_len = normalize_pem_string(ca_cert_pem_raw,
                                                        ca_cert_pem,
                                                        sizeof(ca_cert_pem));

    if (ca_cert_pem_len == 0U) {
        LOG_ERR("PEM normalization failed - empty result");
        return -1;
    }

    LOG_INF("Storing CA certificate: %u bytes (normalized from config)", ca_cert_pem_len);
    int ret = tls_credential_add(CONTROL_TLS_SEC_TAG,
                             TLS_CREDENTIAL_CA_CERTIFICATE,
                             ca_cert_pem,
                             ca_cert_pem_len + 1U);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("TLS CA certificate stored and registered (sec_tag=%d)", CONTROL_TLS_SEC_TAG);
    return 0;
}

static int configure_tls_socket(int sock, const char *server_host)
{
    sec_tag_t sec_tag_list[] = { CONTROL_TLS_SEC_TAG };
    const int verify = TLS_PEER_VERIFY_REQUIRED;
    const struct timeval recv_timeout = {
        .tv_sec = 180,
        .tv_usec = 0,
    };
    int ret;

    ret = setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
                     sec_tag_list, sizeof(sec_tag_list));
    if (ret < 0) {
        return -errno;
    }

    ret = setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
                     server_host, strlen(server_host));
    if (ret < 0) {
        return -errno;
    }

    ret = setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
                     &verify, sizeof(verify));
    if (ret < 0) {
        return -errno;
    }

    ret = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                     &recv_timeout, sizeof(recv_timeout));
    if (ret < 0) {
        return -errno;
    }

    return 0;
}

static bool http_status_is_success(unsigned int status_code)
{
    return (status_code >= 200U) && (status_code < 300U);
}

static int response_cb(struct http_response *rsp,
                       enum http_final_call final_data,
                       void *user_data)
{
    struct upload_context *context = user_data;

    if (rsp == NULL) {
        LOG_ERR("HTTP response is NULL");
        return -EINVAL;
    }

    if (context != NULL) {
        context->http_status_code = rsp->http_status_code;
    }

    if ((rsp->http_status_code > 0U) &&
        ((context == NULL) || !context->http_status_logged)) {
        LOG_INF("HTTP Status: %u (%s)",
                (unsigned int)rsp->http_status_code,
                rsp->http_status != NULL ? rsp->http_status : "");
        if (context != NULL) {
            context->http_status_logged = true;
        }
    }

    if (final_data == HTTP_DATA_MORE) {
        LOG_DBG("HTTP response chunk: %u bytes", (unsigned int)rsp->data_len);
        return 0;
    }

    if (context != NULL) {
        context->response_complete = true;
    }

    LOG_INF("HTTP response complete: status=%u, total processed=%u bytes",
            (unsigned int)rsp->http_status_code,
            (unsigned int)rsp->processed);

    return 0;
}

static int send_buffer_with_retry(int sock, const void *buffer, size_t length)
{
    const uint8_t *data = buffer;
    size_t offset = 0U;

    while (offset < length) {
        const ssize_t ret = send(sock, data + offset, length - offset, 0);

        if (ret > 0) {
            offset += (size_t)ret;
            continue;
        }

        if ((ret < 0) && (errno == EAGAIN || errno == ENOMEM)) {
            k_sleep(K_MSEC(50));
            continue;
        }

        return (ret < 0) ? -errno : -EIO;
    }

    return 0;
}

static int payload_cb(int sock, struct http_request *req, void *user_data)
{
    ARG_UNUSED(req);

    struct upload_context *context = user_data;
    struct upload_progress *progress = &context->progress;
    const uint8_t *payload = one_month_data;
    const size_t total_size = ONE_MONTH_DATA_SIZE;
    size_t offset = 0U;
    int total_sent = 0;

    while (offset < total_size) {
        char chunk_header[16];
        const size_t chunk_size = MIN(CONTROL_HTTP_CHUNK_SIZE, total_size - offset);
        const int header_len = snprintk(chunk_header, sizeof(chunk_header), "%zx\r\n", chunk_size);
        int ret;

        if (header_len <= 0 || header_len >= (int)sizeof(chunk_header)) {
            LOG_ERR("Failed to build chunk header");
            return -ENOMEM;
        }

        ret = send_buffer_with_retry(sock, chunk_header, (size_t)header_len);
        if (ret < 0) {
            LOG_ERR("Failed to send chunk header: %d", ret);
            return ret;
        }

        ret = send_buffer_with_retry(sock, payload + offset, chunk_size);
        if (ret < 0) {
            LOG_ERR("Failed to send chunk payload at offset %u: %d",
                    (unsigned int)offset,
                    ret);
            return ret;
        }

        ret = send_buffer_with_retry(sock, "\r\n", 2U);
        if (ret < 0) {
            LOG_ERR("Failed to send chunk terminator: %d", ret);
            return ret;
        }

        offset += chunk_size;
        progress->bytes_sent = offset;
        progress->chunks_sent++;
        total_sent += header_len + (int)chunk_size + 2;

        if ((progress->chunks_sent == 1U) ||
            ((progress->chunks_sent % 16U) == 0U) ||
            (offset == total_size)) {
            LOG_INF("Chunk progress: %u/%u bytes, chunk %u",
                    (unsigned int)progress->bytes_sent,
                    (unsigned int)progress->total_size,
                    (unsigned int)progress->chunks_sent);
        }
    }

    if (send_buffer_with_retry(sock, "0\r\n\r\n", 5U) < 0) {
        LOG_ERR("Failed to send final chunk terminator");
        return -EIO;
    }

    LOG_INF("Chunked payload transmission complete: %u chunks",
            (unsigned int)progress->chunks_sent);

    return total_sent + 5;
}



static int test_tcp_socket(void)
{
    int sock;
    char port[6];
    uint8_t rx_buf[512];
    const char *server_host = CONFIG_CONTROL_SERVER_HOST;
    const char *server_url = CONFIG_CONTROL_SERVER_URL;
    int ret;
    int64_t connectStartMs;
    int64_t connectElapsedMs;
    static bool tls_credential_added = false;
    struct http_request req;
    struct upload_context context = {
        .progress = {
            .total_size = ONE_MONTH_DATA_SIZE,
            .bytes_sent = 0U,
            .chunks_sent = 0U,
        },
        .http_status_code = 0U,
        .http_status_logged = false,
        .response_complete = false,
    };
    const char *headers[] = {
        "Transfer-Encoding: chunked\r\n",
        "Connection: close\r\n",
        NULL,
    };
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;

    LOG_INF("===== HTTP Upload Flow Start =====");
    LOG_INF("Target: %s%s (%u bytes)", server_host, server_url, ONE_MONTH_DATA_SIZE);
    LOG_INF("Payload preview: %.32s", (const char *)one_month_data);

    if (!tls_credential_added) {
        LOG_INF("Initializing TLS credentials...");
        if (tls_setup() < 0) {
            LOG_ERR("TLS setup failed");
            return -EIO;
        }
        tls_credential_added = true;
    }

    if ((strlen(server_host) == 0U) ||
        (strlen(server_url) == 0U)) {
        LOG_ERR("Server configuration incomplete: host='%s' url='%s'", server_host, server_url);
        LOG_ERR("Check prj.secrets.conf for CONFIG_CONTROL_SERVER_HOST and CONFIG_CONTROL_SERVER_URL");
        return -EINVAL;
    }

    snprintf(port, sizeof(port), "%d", CONFIG_CONTROL_SERVER_PORT);

    /* DNS */
    LOG_INF("DNS lookup: %s:%s", server_host, port);
    ret = getaddrinfo(server_host, port, &hints, &res);
    if (ret != 0) {
        LOG_ERR("DNS lookup failed: error %d", ret);
        return -EHOSTUNREACH;
    }
    LOG_INF("DNS resolved, creating TLS socket...");

    /* Use TLS 1.2 protocol directly in socket creation */
    sock = socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
    if (sock < 0) {
        LOG_ERR("socket() failed: errno=%d", errno);
        freeaddrinfo(res);
        return -errno;
    }
    LOG_INF("TLS socket created, configuring parameters...");

    ret = configure_tls_socket(sock, server_host);
    if (ret < 0) {
        LOG_ERR("TLS socket configuration failed: %d", ret);
        close(sock);
        freeaddrinfo(res);
        return ret;
    }

    LOG_INF("Starting connect (TCP + TLS handshake)...");
    connectStartMs = k_uptime_get();
    ret = connect(sock, res->ai_addr, res->ai_addrlen);
    connectElapsedMs = k_uptime_get() - connectStartMs;
    freeaddrinfo(res);

    if (ret < 0) {
        LOG_ERR("connect() failed: errno=%d", errno);
        close(sock);
        return -errno;
    }

    LOG_INF("Connect + TLS handshake time: %lld ms", connectElapsedMs);
    LOG_INF("TLS 1.2 handshake complete with %s", server_host);

    memset(&req, 0, sizeof(req));

    req.method = HTTP_POST;
    req.url = server_url;
    req.host = server_host;
    req.protocol = "HTTP/1.1";
    req.content_type_value = "application/octet-stream";
    req.header_fields = headers;
    req.payload_cb = payload_cb;
    req.response = response_cb;
    req.recv_buf = rx_buf;
    req.recv_buf_len = sizeof(rx_buf);

    LOG_INF("HTTP POST request: %s, chunked payload=%u bytes, chunk_size=%u, rx_buf=%u bytes",
            server_url,
            ONE_MONTH_DATA_SIZE,
            CONTROL_HTTP_CHUNK_SIZE,
            sizeof(rx_buf));

    ret = http_client_req(sock, &req, CONTROL_HTTP_TIMEOUT_MS, &context);
    if (ret < 0) {
        LOG_ERR("http_client_req() failed: %d", ret);
        close(sock);
        return ret;
    }

    if (!context.response_complete) {
        LOG_ERR("HTTP request finished without a complete response");
        close(sock);
        return -EBADMSG;
    }

    if (!http_status_is_success(context.http_status_code)) {
        LOG_ERR("HTTP request failed with status %u", context.http_status_code);
        close(sock);
        return -EBADMSG;
    }

    LOG_INF("HTTP request complete: tx_bytes=%u, status=%u",
            (unsigned int)ret,
            context.http_status_code);

    close(sock);
    ppp_test_ready = false;

    LOG_INF("===== HTTP Upload Flow Complete =====");

    return 0;
}


/* Net management event handler: triggers HTTP upload test when network is ready */
static void net_event_handler(struct net_mgmt_event_callback *cb,
                              uint64_t mgmt_event,
                              struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if ((mgmt_event == NET_EVENT_PPP_PHASE_RUNNING ||
         mgmt_event == NET_EVENT_IPV4_ADDR_ADD ||
         mgmt_event == NET_EVENT_L4_CONNECTED) && !ppp_test_ready) {
        ppp_test_ready = true;
        LOG_INF("Network ready: initiating HTTP upload test");
    }
}

int main(void)
{
    LOG_INF("control app boot");

    tcp_test_done = false;
    ppp_test_ready = false;

    /* Register network event callbacks */
    net_mgmt_init_event_callback(&net_cb_l2,
                                 net_event_handler,
                                 NET_EVENT_PPP_PHASE_RUNNING);
    net_mgmt_add_event_callback(&net_cb_l2);

    net_mgmt_init_event_callback(&net_cb_l3,
                                 net_event_handler,
                                 NET_EVENT_IPV4_ADDR_ADD |
                                 NET_EVENT_DNS_SERVER_ADD);
    net_mgmt_add_event_callback(&net_cb_l3);

    net_mgmt_init_event_callback(&net_cb_l4,
                                 net_event_handler,
                                 NET_EVENT_L4_CONNECTED);
    net_mgmt_add_event_callback(&net_cb_l4);

    /* Main event loop - send data once when network is ready */
    while (!tcp_test_done) {
        if (ppp_test_ready) {
            const int uploadStatus = test_tcp_socket();

            tcp_test_done = true;
            if (uploadStatus == 0) {
                LOG_INF("Upload finished successfully");
            } else {
                LOG_ERR("Upload finished with error: %d", uploadStatus);
            }

            break;
        }
        k_sleep(K_SECONDS(1));
    }

    LOG_INF("Main loop complete, entering idle loop");
    while (1) {
        k_sleep(K_SECONDS(60));
    }

    return 0;
}
