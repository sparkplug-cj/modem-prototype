#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

#include "modem-link.h"
#include "../one_month.h"

LOG_MODULE_REGISTER(control_app, LOG_LEVEL_INF);

#define CONTROL_TLS_SEC_TAG 1
#define CONTROL_HTTP_TIMEOUT_MS (180 * MSEC_PER_SEC)
#define CONTROL_HTTP_CHUNK_SIZE 1024U
#define CONTROL_HTTP_RESPONSE_PREVIEW_LEN 160U

struct control_http_context {
    unsigned int http_status_code;
    bool http_status_logged;
    bool response_complete;
    size_t bytes_sent;
    size_t chunks_sent;
    size_t preview_len;
    char response_preview[CONTROL_HTTP_RESPONSE_PREVIEW_LEN + 1U];
};

static bool control_app_http_status_is_success(unsigned int status_code)
{
    return (status_code >= 200U) && (status_code < 300U);
}

static void control_app_append_response_preview(struct control_http_context *context,
                                                const uint8_t *fragment,
                                                size_t fragment_len)
{
    size_t idx;

    if ((context == NULL) || (fragment == NULL) || (fragment_len == 0U)) {
        return;
    }

    for (idx = 0U; idx < fragment_len; idx++) {
        const uint8_t ch = fragment[idx];

        if (context->preview_len >= CONTROL_HTTP_RESPONSE_PREVIEW_LEN) {
            break;
        }

        if ((ch >= 0x20U) && (ch <= 0x7eU)) {
            context->response_preview[context->preview_len++] = (char)ch;
        } else if ((ch == '\n') || (ch == '\r') || (ch == '\t')) {
            context->response_preview[context->preview_len++] = ' ';
        } else {
            context->response_preview[context->preview_len++] = '.';
        }
    }

    context->response_preview[context->preview_len] = '\0';
}

static size_t control_app_normalize_pem_string(const char *src,
                                               char *dst,
                                               size_t dst_size)
{
    size_t src_idx = 0U;
    size_t dst_idx = 0U;

    while ((src[src_idx] != '\0') && (dst_idx + 1U < dst_size)) {
        if (src[src_idx] == '\\') {
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

static int control_app_tls_setup(void)
{
    static bool tls_credential_added;
    static char ca_cert_pem[sizeof(CONFIG_CONTROL_TLS_SERVER_CA_CERT_PEM)];
    size_t normalized_len;
    int ret;

    if (tls_credential_added) {
        return 0;
    }

    if (strlen(CONFIG_CONTROL_TLS_SERVER_CA_CERT_PEM) == 0U) {
        LOG_ERR("TLS certificate empty");
        return -EINVAL;
    }

    normalized_len = control_app_normalize_pem_string(CONFIG_CONTROL_TLS_SERVER_CA_CERT_PEM,
                                                      ca_cert_pem,
                                                      sizeof(ca_cert_pem));
    if (normalized_len == 0U) {
        LOG_ERR("PEM normalization failed");
        return -EINVAL;
    }

    ret = tls_credential_add(CONTROL_TLS_SEC_TAG,
                             TLS_CREDENTIAL_CA_CERTIFICATE,
                             ca_cert_pem,
                             normalized_len + 1U);
    if ((ret < 0) && (ret != -EEXIST)) {
        LOG_ERR("tls_credential_add failed: %d", ret);
        return ret;
    }

    tls_credential_added = true;
    return 0;
}

static int control_app_configure_tls_socket(int sock, const char *server_host)
{
    sec_tag_t sec_tag_list[] = { CONTROL_TLS_SEC_TAG };
    const int verify = TLS_PEER_VERIFY_REQUIRED;
    const struct timeval recv_timeout = {
        .tv_sec = CONTROL_HTTP_TIMEOUT_MS / MSEC_PER_SEC,
        .tv_usec = 0,
    };
    int ret;

    ret = zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
                           sec_tag_list, sizeof(sec_tag_list));
    if (ret < 0) {
        return -errno;
    }

    ret = zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
                           server_host, strlen(server_host));
    if (ret < 0) {
        return -errno;
    }

    ret = zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
                           &verify, sizeof(verify));
    if (ret < 0) {
        return -errno;
    }

    ret = zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                           &recv_timeout, sizeof(recv_timeout));
    if (ret < 0) {
        return -errno;
    }

    return 0;
}

static int control_app_response_cb(struct http_response *rsp,
                                   enum http_final_call final_data,
                                   void *user_data)
{
    struct control_http_context *context = user_data;

    if ((rsp == NULL) || (context == NULL)) {
        return -EINVAL;
    }

    context->http_status_code = rsp->http_status_code;

    if ((rsp->http_status_code > 0U) && !context->http_status_logged) {
        LOG_INF("HTTP status: %u %s",
                (unsigned int)rsp->http_status_code,
                rsp->http_status);
        context->http_status_logged = true;
    }

    if ((rsp->body_frag_start != NULL) && (rsp->body_frag_len > 0U)) {
        control_app_append_response_preview(context,
                                            rsp->body_frag_start,
                                            rsp->body_frag_len);
    }

    if (final_data == HTTP_DATA_MORE) {
        LOG_INF("HTTP response progress: processed=%u body=%u bytes",
                (unsigned int)rsp->processed,
                (unsigned int)rsp->body_frag_len);
    }

    if (final_data == HTTP_DATA_FINAL) {
        context->response_complete = true;
        LOG_INF("HTTP response complete: processed=%u bytes",
                (unsigned int)rsp->processed);

        if (context->preview_len > 0U) {
            LOG_INF("HTTP response preview: %s", context->response_preview);
        }
    }

    return 0;
}

static int control_app_send_buffer_with_retry(int sock, const void *buffer, size_t length)
{
    const uint8_t *data = buffer;
    size_t offset = 0U;

    while (offset < length) {
        const ssize_t ret = zsock_send(sock, data + offset, length - offset, 0);

        if (ret > 0) {
            offset += (size_t)ret;
            LOG_INF("============== PARTIAL : %d , length : %d, offset: %d ============================",
                    (int)ret,
                    (int)length,
                    (int)offset);
            continue;
        }

        if ((ret < 0) && ((errno == EAGAIN) || (errno == ENOMEM))) {
            k_sleep(K_MSEC(5));
            LOG_INF("++++++++++++++++RETRY++++++++++++++++++++++");
            continue;
        }

        return (ret < 0) ? -errno : -EIO;
    }

    return 0;
}

static int control_app_payload_cb(int sock, struct http_request *req, void *user_data)
{
    struct control_http_context *context = user_data;
    static uint8_t chunk_buffer[CONTROL_HTTP_CHUNK_SIZE + 16U];
    size_t offset = 0U;
    unsigned int last_logged_pct = 0U;
    int total_sent = 0;

    ARG_UNUSED(req);

    LOG_INF("HTTP upload: %u bytes, chunk_size=%u",
            (unsigned int)ONE_MONTH_DATA_SIZE,
            (unsigned int)CONTROL_HTTP_CHUNK_SIZE);

    while (offset < ONE_MONTH_DATA_SIZE) {
        const size_t chunk_size = MIN(CONTROL_HTTP_CHUNK_SIZE, ONE_MONTH_DATA_SIZE - offset);
        const int header_len = snprintk((char *)chunk_buffer,
                                        sizeof(chunk_buffer),
                                        "%zx\r\n",
                                        chunk_size);
        const size_t chunk_total = (size_t)header_len + chunk_size + 2U;
        unsigned int pct;
        int ret;

        if ((header_len <= 0) || (chunk_total > sizeof(chunk_buffer))) {
            return -ENOMEM;
        }

        memcpy(&chunk_buffer[header_len], one_month_data + offset, chunk_size);
        chunk_buffer[header_len + chunk_size] = '\r';
        chunk_buffer[header_len + chunk_size + 1U] = '\n';

        LOG_INF("Sending chunk: payload_offset=%u payload_len=%u wire_len=%u",
                (unsigned int)offset,
                (unsigned int)chunk_size,
                (unsigned int)chunk_total);

        ret = control_app_send_buffer_with_retry(sock, chunk_buffer, chunk_total);
        if (ret < 0) {
            LOG_ERR("Failed to send chunk at offset %u: %d",
                    (unsigned int)offset,
                    ret);
            return ret;
        }

        offset += chunk_size;
        total_sent += (int)chunk_total;

        if (context != NULL) {
            context->bytes_sent = offset;
            context->chunks_sent++;
        }

        pct = (unsigned int)((offset * 100U) / ONE_MONTH_DATA_SIZE);
        if ((offset == ONE_MONTH_DATA_SIZE) || ((pct / 25U) > (last_logged_pct / 25U))) {
            LOG_INF("Upload progress: %u/%u bytes (%u%%), chunk %u",
                    (unsigned int)offset,
                    (unsigned int)ONE_MONTH_DATA_SIZE,
                    pct,
                    (unsigned int)((context != NULL) ? context->chunks_sent : 0U));
            last_logged_pct = pct;
        }
    }

    {
        LOG_INF("Sending final chunk terminator");
        const int ret = control_app_send_buffer_with_retry(sock, "0\r\n\r\n", 5U);

        if (ret < 0) {
            LOG_ERR("Failed to send final chunk terminator");
            return ret;
        }
    }

    LOG_INF("Chunked payload transmission complete: %u chunks, %u payload bytes",
            (unsigned int)((context != NULL) ? context->chunks_sent : 0U),
            (unsigned int)ONE_MONTH_DATA_SIZE);

    return total_sent + 5;
}

static int control_app_send_https_upload(void)
{
    const char *server_host = CONFIG_CONTROL_SERVER_HOST;
    const char *server_url = CONFIG_CONTROL_SERVER_URL;
    const char *headers[] = {
        "Transfer-Encoding: chunked\r\n",
        "Connection: close\r\n",
        NULL,
    };
    struct zsock_addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct zsock_addrinfo *res = NULL;
    struct http_request req = { 0 };
    struct control_http_context context = { 0 };
    uint8_t rx_buf[512];
    char port[6];
    int64_t connect_start_ms;
    int64_t connect_elapsed_ms;
    int64_t request_start_ms;
    int64_t request_elapsed_ms;
    int sock = -1;
    int ret;

    if ((strlen(server_host) == 0U) || (strlen(server_url) == 0U)) {
        LOG_ERR("Server configuration incomplete");
        return -EINVAL;
    }

    ret = control_app_tls_setup();
    if (ret < 0) {
        return ret;
    }

    ret = snprintf(port, sizeof(port), "%d", CONFIG_CONTROL_SERVER_PORT);
    if ((ret <= 0) || (ret >= (int)sizeof(port))) {
        return -EINVAL;
    }

    LOG_INF("HTTPS upload target: %s:%s%s", server_host, port, server_url);
    LOG_INF("Payload size: %u bytes", (unsigned int)ONE_MONTH_DATA_SIZE);
    LOG_INF("HTTP upload mode: chunked payload callback (%u-byte chunks)",
        (unsigned int)CONTROL_HTTP_CHUNK_SIZE);

    ret = zsock_getaddrinfo(server_host, port, &hints, &res);
    if (ret != 0) {
        LOG_ERR("DNS lookup failed: %d", ret);
        return -EHOSTUNREACH;
    }

    sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
    if (sock < 0) {
        ret = -errno;
        zsock_freeaddrinfo(res);
        return ret;
    }

    ret = control_app_configure_tls_socket(sock, server_host);
    if (ret < 0) {
        zsock_close(sock);
        zsock_freeaddrinfo(res);
        return ret;
    }

    connect_start_ms = k_uptime_get();
    ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
    connect_elapsed_ms = k_uptime_get() - connect_start_ms;
    zsock_freeaddrinfo(res);
    if (ret < 0) {
        ret = -errno;
        zsock_close(sock);
        return ret;
    }

    LOG_INF("TLS connect complete in %lld ms", connect_elapsed_ms);

    req.method = HTTP_POST;
    req.url = server_url;
    req.protocol = "HTTP/1.1";
    req.host = server_host;
    req.header_fields = headers;
    req.content_type_value = "application/octet-stream";
    req.payload_cb = control_app_payload_cb;
    req.payload = NULL;
    req.payload_len = 0U;
    req.response = control_app_response_cb;
    req.recv_buf = rx_buf;
    req.recv_buf_len = sizeof(rx_buf);

        LOG_INF("HTTP POST request: %s, chunked payload=%u bytes, chunk_size=%u, rx_buf=%u bytes",
            server_url,
            (unsigned int)ONE_MONTH_DATA_SIZE,
            (unsigned int)CONTROL_HTTP_CHUNK_SIZE,
            (unsigned int)sizeof(rx_buf));

    request_start_ms = k_uptime_get();
    ret = http_client_req(sock, &req, CONTROL_HTTP_TIMEOUT_MS, &context);
    request_elapsed_ms = k_uptime_get() - request_start_ms;
    zsock_close(sock);
    if (ret < 0) {
        LOG_ERR("Upload failure stats: sent=%u payload bytes across %u chunks",
                (unsigned int)context.bytes_sent,
                (unsigned int)context.chunks_sent);
        LOG_ERR("http_client_req failed after %lld ms: %d", request_elapsed_ms, ret);
        return ret;
    }

    if (!context.response_complete) {
        return -EBADMSG;
    }

    LOG_INF("HTTP upload sent %u on-wire bytes in %lld ms",
            (unsigned int)ret,
            request_elapsed_ms);
    LOG_INF("HTTP upload payload bytes: %u across %u chunks",
            (unsigned int)context.bytes_sent,
            (unsigned int)context.chunks_sent);

    if (request_elapsed_ms > 0) {
        const uint64_t bytes_per_sec = ((uint64_t)context.bytes_sent * 1000U) /
                                       (uint64_t)request_elapsed_ms;
        LOG_INF("HTTP upload payload throughput: %llu B/s", bytes_per_sec);
    }

    if (!control_app_http_status_is_success(context.http_status_code)) {
        LOG_ERR("Upload returned non-2xx status: %u", context.http_status_code);
        return -EBADMSG;
    }

    LOG_INF("Upload status: %u", context.http_status_code);
    return 0;
}

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

    net_if_set_default(pppIface);

    ret = control_app_send_https_upload();
    if (ret != 0) {
        LOG_ERR("HTTPS upload failed: %d", ret);
    }

    ret = conn_mgr_if_disconnect(pppIface);
    if (ret != 0) {
        LOG_ERR("conn_mgr_if_disconnect failed: %d", ret);
        return 0;
    }

    LOG_INF("PPP disconnected");
    
    while (1) {
        k_sleep(K_FOREVER);
    }
}
