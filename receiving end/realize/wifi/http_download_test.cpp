#include "http_download_test.hpp"

#include "http_utils.hpp"
#include "wifi_task.hpp"

extern "C" {
#include "lwip/sockets.h"
#include "soc_osal.h"
#include "systick.h"
#include "trng.h"
}

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>

namespace {
// Temporary relay throughput target. The Ubuntu server downloads the QQ file
// first and exposes it as a local static MP3 for the WS63-side test.
constexpr char k_host[] = "124.222.12.152";
constexpr char k_path[] = "/ws63-test.mp3";
constexpr bool k_use_tls = false;
constexpr uint16_t k_port = 18080;
constexpr unsigned int k_test_count = 5;
constexpr unsigned int k_parallel_count = 1;
// This test sends Range: bytes=0- specifically to verify that the relay keeps
// the partial-content contract needed by pause/resume and seek.
constexpr bool k_require_partial_content = true;
constexpr uint32_t k_wifi_wait_timeout_ms = 60000;
constexpr uint32_t k_dns_timeout_ms = 60000;
constexpr uint32_t k_tls_handshake_timeout_ms = 10000;
constexpr uint32_t k_header_timeout_ms = 5000;
constexpr uint32_t k_download_timeout_ms = 60000;
constexpr uint32_t k_body_read_timeout_ms = 1000;
constexpr size_t k_tls_entropy_min_hardclock = 4;
constexpr size_t k_recv_buffer_size = 8192;
constexpr size_t k_header_buffer_size = 4096;

uint8_t g_recv_buffer[k_recv_buffer_size];
char g_header_buffer[k_header_buffer_size];
char g_request_buffer[1536];

struct tls_transport {
    bool initialized = false;
    bool use_tls = false;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
};

struct test_result {
    bool valid_response = false;
    bool reached_eof = false;
    uint64_t total_bytes = 0;
    uint64_t elapsed_ms = 0;
    uint64_t average_kbps = 0;
};

struct parallel_connection {
    tls_transport transport = {};
    char header[k_header_buffer_size] = {0};
    char server_ip_text[20] = {0};
    size_t header_length = 0;
    uint64_t phase_start_ms = 0;
    uint64_t report_start_ms = 0;
    uint64_t download_start_ms = 0;
    uint64_t total_bytes = 0;
    uint64_t interval_bytes = 0;
    uint64_t expected_bytes = 0;
    uint64_t report_count = 0;
    uint64_t low_rate_count = 0;
    uint64_t zero_rate_count = 0;
    uint64_t min_nonzero_kbps = UINT64_MAX;
    uint64_t max_interval_kbps = 0;
    bool active = false;
    bool header_done = false;
    bool valid_response = false;
    bool reached_eof = false;
};

parallel_connection g_parallel_connections[k_parallel_count];

uint64_t elapsed_ms(uint64_t start_ms)
{
    const uint64_t now_ms = uapi_systick_get_ms();
    if (now_ms >= start_ms) {
        return now_ms - start_ms;
    }
    return (UINT64_MAX - start_ms) + now_ms + 1;
}

bool has_valid_ipv4()
{
    const char *ip = wifi_get_current_ip();
    return ip != nullptr && ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0;
}

bool wait_for_wifi()
{
    const uint64_t wait_start = uapi_systick_get_ms();
    while (!wifi_is_sta_connected() || !has_valid_ipv4()) {
        if (elapsed_ms(wait_start) >= k_wifi_wait_timeout_ms) {
            osal_printk("[HTTP-DL] DHCP wait timed out, ip=%s\r\n", wifi_get_current_ip());
            return false;
        }
        osal_msleep(200);
    }
    return true;
}

int tls_entropy_source(void *context, unsigned char *output, size_t length, size_t *output_length)
{
    (void)context;
    if (output == nullptr || output_length == nullptr || length == 0 || length > UINT32_MAX) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    if (uapi_drv_cipher_trng_get_random_bytes(output, static_cast<uint32_t>(length)) != ERRCODE_SUCC) {
        *output_length = 0;
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    *output_length = length;
    return 0;
}

void close_transport(tls_transport &transport)
{
    if (!transport.initialized) {
        return;
    }
    if (transport.use_tls) {
        (void)mbedtls_ssl_close_notify(&transport.ssl);
    }
    mbedtls_ssl_free(&transport.ssl);
    mbedtls_ssl_config_free(&transport.config);
    mbedtls_ctr_drbg_free(&transport.ctr_drbg);
    mbedtls_entropy_free(&transport.entropy);
    mbedtls_net_free(&transport.net);
    transport.initialized = false;
}

bool resolve_server(in_addr &server_ip)
{
    const uint64_t resolve_start = uapi_systick_get_ms();
    while (!resolve_ipv4_addr(k_host, &server_ip)) {
        if (elapsed_ms(resolve_start) >= k_dns_timeout_ms) {
            return false;
        }
        osal_msleep(1000);
    }
    return true;
}

bool open_transport(tls_transport &transport, const in_addr &server_ip, unsigned int test_index,
                    uint64_t &tcp_connect_ms, uint64_t &tls_handshake_ms)
{
    mbedtls_net_init(&transport.net);
    mbedtls_ssl_init(&transport.ssl);
    mbedtls_ssl_config_init(&transport.config);
    mbedtls_ctr_drbg_init(&transport.ctr_drbg);
    mbedtls_entropy_init(&transport.entropy);
    transport.initialized = true;
    transport.use_tls = k_use_tls;

    int ret = 0;
    if (transport.use_tls) {
        ret = mbedtls_entropy_add_source(&transport.entropy, tls_entropy_source, nullptr,
                                         k_tls_entropy_min_hardclock, MBEDTLS_ENTROPY_SOURCE_STRONG);
        static const char k_personalization[] = "ws63-http-download-test";
        if (ret == 0) {
            ret = mbedtls_ctr_drbg_seed(&transport.ctr_drbg, mbedtls_entropy_func, &transport.entropy,
                                        reinterpret_cast<const unsigned char *>(k_personalization),
                                        strlen(k_personalization));
        }
        if (ret != 0) {
            osal_printk("[HTTP-DL][%u/%u] TLS random init failed: ret=-0x%04X\r\n", test_index, k_test_count,
                        -ret);
            return false;
        }

        ret = mbedtls_ssl_config_defaults(&transport.config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) {
            osal_printk("[HTTP-DL][%u/%u] TLS config failed: ret=-0x%04X\r\n", test_index, k_test_count, -ret);
            return false;
        }

        mbedtls_ssl_conf_authmode(&transport.config, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&transport.config, mbedtls_ctr_drbg_random, &transport.ctr_drbg);
        mbedtls_ssl_conf_min_version(&transport.config, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
        mbedtls_ssl_conf_max_version(&transport.config, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
        mbedtls_ssl_conf_read_timeout(&transport.config, 10);

        ret = mbedtls_ssl_setup(&transport.ssl, &transport.config);
        if (ret == 0) {
            ret = mbedtls_ssl_set_hostname(&transport.ssl, k_host);
        }
        if (ret != 0) {
            osal_printk("[HTTP-DL][%u/%u] TLS setup/SNI failed: ret=-0x%04X\r\n", test_index, k_test_count, -ret);
            return false;
        }
    }

    transport.net.fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (transport.net.fd < 0) {
        osal_printk("[HTTP-DL][%u/%u] socket failed: errno=%d\r\n", test_index, k_test_count, errno);
        return false;
    }

    timeval send_timeout = {5, 0};
    (void)lwip_setsockopt(transport.net.fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = lwip_htons(k_port);
    server_addr.sin_addr = server_ip;
    const uint64_t tcp_start = uapi_systick_get_ms();
    if (lwip_connect(transport.net.fd, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) < 0) {
        osal_printk("[HTTP-DL][%u/%u] TCP connect failed: errno=%d\r\n", test_index, k_test_count, errno);
        return false;
    }
    tcp_connect_ms = elapsed_ms(tcp_start);

    if (!transport.use_tls) {
        tls_handshake_ms = 0;
        return true;
    }

    mbedtls_ssl_set_bio(&transport.ssl, &transport.net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
    const uint64_t handshake_start = uapi_systick_get_ms();
    do {
        ret = mbedtls_ssl_handshake(&transport.ssl);
        if (ret == 0) {
            tls_handshake_ms = elapsed_ms(handshake_start);
            return true;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE &&
            ret != MBEDTLS_ERR_SSL_TIMEOUT) {
            break;
        }
    } while (elapsed_ms(handshake_start) < k_tls_handshake_timeout_ms);

    tls_handshake_ms = elapsed_ms(handshake_start);
    osal_printk("[HTTP-DL][%u/%u] TLS handshake failed: ret=-0x%04X elapsed=%llu ms\r\n", test_index,
                k_test_count, -ret, static_cast<unsigned long long>(tls_handshake_ms));
    return false;
}

bool tls_send_all(tls_transport &transport, const char *data, size_t length, unsigned int test_index)
{
    size_t sent = 0;
    while (sent < length) {
        const int ret = transport.use_tls
                            ? mbedtls_ssl_write(&transport.ssl,
                                                reinterpret_cast<const unsigned char *>(data) + sent, length - sent)
                            : lwip_send(transport.net.fd, data + sent, static_cast<int>(length - sent), 0);
        if (ret > 0) {
            sent += static_cast<size_t>(ret);
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
            ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            osal_msleep(1);
            continue;
        }
        osal_printk("[HTTP-DL][%u/%u] request send failed: ret=%d errno=%d\r\n", test_index, k_test_count, ret,
                    errno);
        return false;
    }
    return true;
}

int tls_recv(tls_transport &transport, void *buffer, size_t length, bool &timed_out)
{
    timed_out = false;
    if (!transport.use_tls) {
        const int ret = lwip_recv(transport.net.fd, buffer, static_cast<int>(length), 0);
        if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            timed_out = true;
        }
        return ret;
    }
    const int ret = mbedtls_ssl_read(&transport.ssl, static_cast<unsigned char *>(buffer), length);
    if (ret > 0) {
        return ret;
    }
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
    }
    if (ret == MBEDTLS_ERR_SSL_TIMEOUT) {
        timed_out = true;
        return -1;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return -2;
    }
    return ret;
}

void set_read_timeout(tls_transport &transport, uint32_t timeout_ms)
{
    if (transport.use_tls) {
        mbedtls_ssl_conf_read_timeout(&transport.config, timeout_ms);
        return;
    }
    timeval timeout = {static_cast<long>(timeout_ms / 1000U),
                       static_cast<long>((timeout_ms % 1000U) * 1000U)};
    (void)lwip_setsockopt(transport.net.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

void print_rate(unsigned int test_index, uint64_t interval_bytes, uint64_t interval_ms, uint64_t total_bytes,
                uint64_t total_ms)
{
    if (interval_ms == 0 || total_ms == 0) {
        return;
    }
    const uint64_t interval_kbps = interval_bytes * 8ULL / interval_ms;
    const uint64_t average_kbps = total_bytes * 8ULL / total_ms;
    osal_printk("[HTTP-DL][%u/%u] recv=%llu bytes/%llu ms, rate=%llu kbps, total=%llu bytes, avg=%llu kbps\r\n",
                test_index, k_test_count, static_cast<unsigned long long>(interval_bytes),
                static_cast<unsigned long long>(interval_ms), static_cast<unsigned long long>(interval_kbps),
                static_cast<unsigned long long>(total_bytes), static_cast<unsigned long long>(average_kbps));
}

void copy_header_value(const char *header, const char *key, char *out, size_t out_size)
{
    if (!extract_http_header_value(header, key, out, out_size) && out_size > 0) {
        out[0] = '\0';
    }
}

[[maybe_unused]] test_result run_test(unsigned int test_index)
{
    test_result result = {};
    if (!wait_for_wifi()) {
        return result;
    }

    osal_printk("[HTTP-DL][%u/%u] starting browser-audio profile, host=%s\r\n", test_index, k_test_count, k_host);
    in_addr server_ip = {};
    if (!resolve_server(server_ip)) {
        osal_printk("[HTTP-DL][%u/%u] DNS resolve timed out\r\n", test_index, k_test_count);
        return result;
    }

    char server_ip_text[20] = {0};
    if (lwip_inet_ntop(AF_INET, &server_ip, server_ip_text, sizeof(server_ip_text)) == nullptr) {
        (void)snprintf(server_ip_text, sizeof(server_ip_text), "unknown");
    }
    osal_printk("[HTTP-DL][%u/%u] DNS %s -> %s\r\n", test_index, k_test_count, k_host, server_ip_text);

    tls_transport transport = {};
    uint64_t tcp_connect_ms = 0;
    uint64_t tls_handshake_ms = 0;
    if (!open_transport(transport, server_ip, test_index, tcp_connect_ms, tls_handshake_ms)) {
        close_transport(transport);
        return result;
    }
    osal_printk("[HTTP-DL][%u/%u] connected: tcp=%llu ms\r\n", test_index, k_test_count,
                static_cast<unsigned long long>(tcp_connect_ms));

    const int request_length = snprintf(g_request_buffer, sizeof(g_request_buffer),
                                        "GET %s HTTP/1.1\r\n"
                                        "Host: %s\r\n"
                                        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36\r\n"
                                        "Accept: audio/mpeg,audio/*;q=0.9,*/*;q=0.8\r\n"
                                        "Accept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n"
                                        "Accept-Encoding: identity\r\n"
                                        "Range: bytes=0-\r\n"
                                        "Referer: https://y.qq.com/\r\n"
                                        "Sec-CH-UA: \"Not.A/Brand\";v=\"99\", \"Google Chrome\";v=\"138\", \"Chromium\";v=\"138\"\r\n"
                                        "Sec-CH-UA-Mobile: ?0\r\n"
                                        "Sec-CH-UA-Platform: \"Windows\"\r\n"
                                        "Sec-Fetch-Dest: audio\r\n"
                                        "Sec-Fetch-Mode: no-cors\r\n"
                                        "Sec-Fetch-Site: cross-site\r\n"
                                        "Connection: keep-alive\r\n\r\n",
                                        k_path, k_host);
    if (request_length <= 0 || static_cast<size_t>(request_length) >= sizeof(g_request_buffer) ||
        !tls_send_all(transport, g_request_buffer, static_cast<size_t>(request_length), test_index)) {
        close_transport(transport);
        return result;
    }

    set_read_timeout(transport, k_header_timeout_ms);
    size_t header_length = 0;
    char *header_end = nullptr;
    while (header_length < sizeof(g_header_buffer) - 1) {
        bool timed_out = false;
        const int ret = tls_recv(transport, g_header_buffer + header_length,
                                 sizeof(g_header_buffer) - 1 - header_length, timed_out);
        if (ret == -2) {
            osal_msleep(1);
            continue;
        }
        if (ret <= 0) {
            osal_printk("[HTTP-DL][%u/%u] header recv failed: ret=%d timeout=%d\r\n", test_index,
                        k_test_count, ret, timed_out ? 1 : 0);
            close_transport(transport);
            return result;
        }
        header_length += static_cast<size_t>(ret);
        g_header_buffer[header_length] = '\0';
        header_end = strstr(g_header_buffer, "\r\n\r\n");
        if (header_end != nullptr) {
            break;
        }
    }

    if (header_end == nullptr) {
        osal_printk("[HTTP-DL][%u/%u] response header too large or incomplete\r\n", test_index, k_test_count);
        close_transport(transport);
        return result;
    }

    const size_t response_header_size = static_cast<size_t>(header_end - g_header_buffer) + 4;
    const size_t first_body_size = header_length - response_header_size;
    *header_end = '\0';
    int status_code = 0;
    (void)sscanf(g_header_buffer, "HTTP/%*u.%*u %d", &status_code);
    osal_printk("[HTTP-DL][%u/%u] response:\r\n%s\r\n", test_index, k_test_count, g_header_buffer);
    osal_printk("[HTTP-DL][%u/%u] header=%u bytes, body already received=%u, status=%d\r\n", test_index,
                k_test_count, static_cast<unsigned int>(response_header_size),
                static_cast<unsigned int>(first_body_size), status_code);
    if ((k_require_partial_content && status_code != 206) ||
        (!k_require_partial_content && status_code != 200 && status_code != 206)) {
        osal_printk("[HTTP-DL][%u/%u] unexpected HTTP status (need %s); trial not counted as valid\r\n", test_index,
                    k_test_count, k_require_partial_content ? "206 Partial Content" : "200/206");
        close_transport(transport);
        return result;
    }
    result.valid_response = true;

    char edge_ip[64] = {0};
    char request_uuid[96] = {0};
    char client_ip[64] = {0};
    char cache_lookup[64] = {0};
    char content_length[32] = {0};
    char response_date[64] = {0};
    copy_header_value(g_header_buffer, "X-ServerIp", edge_ip, sizeof(edge_ip));
    copy_header_value(g_header_buffer, "X-NWS-LOG-UUID", request_uuid, sizeof(request_uuid));
    copy_header_value(g_header_buffer, "Client-Ip", client_ip, sizeof(client_ip));
    copy_header_value(g_header_buffer, "X-Cache-Lookup", cache_lookup, sizeof(cache_lookup));
    copy_header_value(g_header_buffer, "Content-Length", content_length, sizeof(content_length));
    copy_header_value(g_header_buffer, "Date", response_date, sizeof(response_date));

    set_read_timeout(transport, k_body_read_timeout_ms);
    uint64_t total_bytes = first_body_size;
    uint64_t interval_bytes = first_body_size;
    const uint64_t download_start = uapi_systick_get_ms();
    uint64_t report_start = download_start;
    uint64_t report_count = 0;
    uint64_t low_rate_count = 0;
    uint64_t zero_rate_count = 0;
    uint64_t min_nonzero_kbps = UINT64_MAX;
    uint64_t max_interval_kbps = 0;
    bool reached_eof = false;

    while (elapsed_ms(download_start) < k_download_timeout_ms) {
        bool timed_out = false;
        const int ret = tls_recv(transport, g_recv_buffer, sizeof(g_recv_buffer), timed_out);
        if (ret > 0) {
            total_bytes += static_cast<uint64_t>(ret);
            interval_bytes += static_cast<uint64_t>(ret);
        } else if (ret == 0) {
            reached_eof = true;
        } else if (ret == -2) {
            osal_msleep(1);
        } else if (!timed_out) {
            osal_printk("[HTTP-DL][%u/%u] body recv failed: ret=%d errno=%d\r\n", test_index, k_test_count,
                        ret, errno);
            break;
        }

        const uint64_t report_elapsed = elapsed_ms(report_start);
        const uint64_t total_elapsed = elapsed_ms(download_start);
        if (report_elapsed >= 1000 || reached_eof) {
            const uint64_t interval_kbps = (report_elapsed > 0) ? (interval_bytes * 8ULL / report_elapsed) : 0;
            ++report_count;
            if (interval_kbps < 128) {
                ++low_rate_count;
            }
            if (interval_bytes == 0) {
                ++zero_rate_count;
            } else if (interval_kbps < min_nonzero_kbps) {
                min_nonzero_kbps = interval_kbps;
            }
            if (interval_kbps > max_interval_kbps) {
                max_interval_kbps = interval_kbps;
            }
            print_rate(test_index, interval_bytes, report_elapsed, total_bytes, total_elapsed);
            interval_bytes = 0;
            report_start = uapi_systick_get_ms();
        }
        if (reached_eof) {
            break;
        }
    }

    result.total_bytes = total_bytes;
    result.elapsed_ms = elapsed_ms(download_start);
    result.average_kbps = (result.elapsed_ms > 0) ? (total_bytes * 8ULL / result.elapsed_ms) : 0;
    result.reached_eof = reached_eof;
    if (min_nonzero_kbps == UINT64_MAX) {
        min_nonzero_kbps = 0;
    }

    osal_printk("[HTTP-DL][%u/%u] META dns_ip=%s edge=%s uuid=%s client=%s cache=%s length=%s date=%s\r\n",
                test_index, k_test_count, server_ip_text, edge_ip, request_uuid, client_ip, cache_lookup,
                content_length, response_date);
    osal_printk("[HTTP-DL][%u/%u] FINISHED total=%llu bytes elapsed=%llu ms avg=%llu kbps eof=%d\r\n", test_index,
                k_test_count, static_cast<unsigned long long>(result.total_bytes),
                static_cast<unsigned long long>(result.elapsed_ms),
                static_cast<unsigned long long>(result.average_kbps), result.reached_eof ? 1 : 0);
    osal_printk("[HTTP-DL][%u/%u] SUMMARY reports=%llu low(<128)=%llu zero=%llu min_nonzero=%llu kbps max=%llu kbps\r\n",
                test_index, k_test_count, static_cast<unsigned long long>(report_count),
                static_cast<unsigned long long>(low_rate_count), static_cast<unsigned long long>(zero_rate_count),
                static_cast<unsigned long long>(min_nonzero_kbps),
                static_cast<unsigned long long>(max_interval_kbps));

    close_transport(transport);
    return result;
}

void print_parallel_rate(unsigned int connection_index, const parallel_connection &connection,
                         uint64_t interval_ms, uint64_t total_ms)
{
    if (interval_ms == 0 || total_ms == 0) {
        return;
    }
    const uint64_t interval_kbps = connection.interval_bytes * 8ULL / interval_ms;
    const uint64_t average_kbps = connection.total_bytes * 8ULL / total_ms;
    osal_printk("[HTTP-DL][P%u/%u] recv=%llu bytes/%llu ms, rate=%llu kbps, total=%llu bytes, avg=%llu kbps\r\n",
                connection_index, k_parallel_count, static_cast<unsigned long long>(connection.interval_bytes),
                static_cast<unsigned long long>(interval_ms), static_cast<unsigned long long>(interval_kbps),
                static_cast<unsigned long long>(connection.total_bytes),
                static_cast<unsigned long long>(average_kbps));
}

bool set_nonblocking(int sock)
{
    int flags = lwip_fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        flags = 0;
    }
    return lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
}

void close_parallel_connection(parallel_connection &connection)
{
    close_transport(connection.transport);
    connection.active = false;
}

bool parse_parallel_header(parallel_connection &connection, unsigned int connection_index)
{
    char *header_end = strstr(connection.header, "\r\n\r\n");
    if (header_end == nullptr) {
        return false;
    }

    const size_t response_header_size = static_cast<size_t>(header_end - connection.header) + 4;
    const size_t first_body_size = connection.header_length - response_header_size;
    *header_end = '\0';
    int status_code = 0;
    (void)sscanf(connection.header, "HTTP/%*u.%*u %d", &status_code);
    osal_printk("[HTTP-DL][P%u/%u] response:\r\n%s\r\n", connection_index, k_parallel_count,
                connection.header);
    osal_printk("[HTTP-DL][P%u/%u] header=%u bytes, body already received=%u, status=%d\r\n", connection_index,
                k_parallel_count, static_cast<unsigned int>(response_header_size),
                static_cast<unsigned int>(first_body_size), status_code);
    if ((k_require_partial_content && status_code != 206) ||
        (!k_require_partial_content && status_code != 200 && status_code != 206)) {
        osal_printk("[HTTP-DL][P%u/%u] unexpected HTTP status (need %s)\r\n", connection_index,
                    k_parallel_count, k_require_partial_content ? "206 Partial Content" : "200/206");
        return false;
    }

    connection.valid_response = true;
    connection.header_done = true;
    char content_length[32] = {0};
    copy_header_value(connection.header, "Content-Length", content_length, sizeof(content_length));
    if (content_length[0] != '\0') {
        connection.expected_bytes = strtoull(content_length, nullptr, 10);
    }
    connection.total_bytes = first_body_size;
    connection.interval_bytes = first_body_size;
    connection.download_start_ms = uapi_systick_get_ms();
    connection.report_start_ms = connection.download_start_ms;
    return true;
}

void print_parallel_summary(unsigned int connection_index, parallel_connection &connection)
{
    const uint64_t total_ms = (connection.download_start_ms != 0) ?
                                  elapsed_ms(connection.download_start_ms) : 0;
    if (connection.header_done && connection.report_start_ms != 0 && connection.interval_bytes > 0) {
        const uint64_t report_ms = elapsed_ms(connection.report_start_ms);
        if (report_ms > 0) {
            const uint64_t interval_kbps = connection.interval_bytes * 8ULL / report_ms;
            ++connection.report_count;
            if (interval_kbps < 128) {
                ++connection.low_rate_count;
            }
            if (interval_kbps > 0 && interval_kbps < connection.min_nonzero_kbps) {
                connection.min_nonzero_kbps = interval_kbps;
            }
            if (interval_kbps > connection.max_interval_kbps) {
                connection.max_interval_kbps = interval_kbps;
            }
            print_parallel_rate(connection_index, connection, report_ms, total_ms);
            connection.interval_bytes = 0;
        }
    }

    if (connection.min_nonzero_kbps == UINT64_MAX) {
        connection.min_nonzero_kbps = 0;
    }
    const uint64_t average_kbps = (total_ms > 0) ? (connection.total_bytes * 8ULL / total_ms) : 0;
    osal_printk("[HTTP-DL][P%u/%u] FINISHED total=%llu bytes elapsed=%llu ms avg=%llu kbps eof=%d\r\n",
                connection_index, k_parallel_count, static_cast<unsigned long long>(connection.total_bytes),
                static_cast<unsigned long long>(total_ms), static_cast<unsigned long long>(average_kbps),
                connection.reached_eof ? 1 : 0);
    osal_printk("[HTTP-DL][P%u/%u] SUMMARY reports=%llu low(<128)=%llu zero=%llu min_nonzero=%llu kbps max=%llu kbps\r\n",
                connection_index, k_parallel_count, static_cast<unsigned long long>(connection.report_count),
                static_cast<unsigned long long>(connection.low_rate_count),
                static_cast<unsigned long long>(connection.zero_rate_count),
                static_cast<unsigned long long>(connection.min_nonzero_kbps),
                static_cast<unsigned long long>(connection.max_interval_kbps));
}

void run_parallel_tests()
{
    if (!wait_for_wifi()) {
        return;
    }

    in_addr server_ip = {};
    if (!resolve_server(server_ip)) {
        osal_printk("[HTTP-DL] parallel DNS resolve timed out\r\n");
        return;
    }
    char server_ip_text[20] = {0};
    if (lwip_inet_ntop(AF_INET, &server_ip, server_ip_text, sizeof(server_ip_text)) == nullptr) {
        (void)snprintf(server_ip_text, sizeof(server_ip_text), "unknown");
    }
    osal_printk("[HTTP-DL] starting %u connection(s) to %s (%s)\r\n", k_parallel_count, k_host,
                server_ip_text);

    unsigned int active_count = 0;
    for (unsigned int index = 0; index < k_parallel_count; ++index) {
        parallel_connection &connection = g_parallel_connections[index];
        close_parallel_connection(connection);
        connection = {};
        (void)snprintf(connection.server_ip_text, sizeof(connection.server_ip_text), "%s", server_ip_text);

        uint64_t tcp_connect_ms = 0;
        uint64_t tls_handshake_ms = 0;
        if (!open_transport(connection.transport, server_ip, index + 1, tcp_connect_ms, tls_handshake_ms)) {
            close_parallel_connection(connection);
            continue;
        }

        const int request_length = snprintf(g_request_buffer, sizeof(g_request_buffer),
                                            "GET %s HTTP/1.1\r\n"
                                            "Host: %s\r\n"
                                            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                            "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36\r\n"
                                            "Accept: audio/mpeg,audio/*;q=0.9,*/*;q=0.8\r\n"
                                            "Accept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n"
                                            "Accept-Encoding: identity\r\n"
                                            "Range: bytes=0-\r\n"
                                            "Referer: https://y.qq.com/\r\n"
                                            "Sec-CH-UA: \"Not.A/Brand\";v=\"99\", \"Google Chrome\";v=\"138\", \"Chromium\";v=\"138\"\r\n"
                                            "Sec-CH-UA-Mobile: ?0\r\n"
                                            "Sec-CH-UA-Platform: \"Windows\"\r\n"
                                            "Sec-Fetch-Dest: audio\r\n"
                                            "Sec-Fetch-Mode: no-cors\r\n"
                                            "Sec-Fetch-Site: cross-site\r\n"
                                            "Connection: keep-alive\r\n\r\n",
                                            k_path, k_host);
        if (request_length <= 0 || static_cast<size_t>(request_length) >= sizeof(g_request_buffer) ||
            !tls_send_all(connection.transport, g_request_buffer, static_cast<size_t>(request_length), index + 1) ||
            !set_nonblocking(connection.transport.net.fd)) {
            osal_printk("[HTTP-DL][P%u/%u] request/nonblocking setup failed\r\n", index + 1, k_parallel_count);
            close_parallel_connection(connection);
            continue;
        }

        connection.active = true;
        connection.phase_start_ms = uapi_systick_get_ms();
        ++active_count;
        sockaddr_in local_addr = {};
        socklen_t local_addr_length = sizeof(local_addr);
        unsigned int local_port = 0;
        if (lwip_getsockname(connection.transport.net.fd, reinterpret_cast<sockaddr *>(&local_addr),
                             &local_addr_length) == 0) {
            local_port = lwip_ntohs(local_addr.sin_port);
        }
        osal_printk("[HTTP-DL][P%u/%u] connected tcp=%llu ms local_port=%u\r\n", index + 1, k_parallel_count,
                    static_cast<unsigned long long>(tcp_connect_ms), local_port);
    }

    const uint64_t wall_start_ms = uapi_systick_get_ms();
    while (active_count > 0 && elapsed_ms(wall_start_ms) < k_download_timeout_ms) {
        bool made_progress = false;
        for (unsigned int index = 0; index < k_parallel_count; ++index) {
            parallel_connection &connection = g_parallel_connections[index];
            if (!connection.active) {
                continue;
            }

            if (!connection.header_done) {
                bool timed_out = false;
                const int ret = tls_recv(connection.transport, connection.header + connection.header_length,
                                         sizeof(connection.header) - 1 - connection.header_length, timed_out);
                if (ret > 0) {
                    connection.header_length += static_cast<size_t>(ret);
                    connection.header[connection.header_length] = '\0';
                    made_progress = true;
                    const bool header_complete = strstr(connection.header, "\r\n\r\n") != nullptr;
                    if (!parse_parallel_header(connection, index + 1) && header_complete) {
                        close_parallel_connection(connection);
                        --active_count;
                        continue;
                    }
                } else if (ret == 0 || (!timed_out && ret != -2)) {
                    osal_printk("[HTTP-DL][P%u/%u] header receive failed ret=%d errno=%d\r\n", index + 1,
                                k_parallel_count, ret, errno);
                    close_parallel_connection(connection);
                    --active_count;
                    continue;
                } else if (elapsed_ms(connection.phase_start_ms) >= k_header_timeout_ms) {
                    osal_printk("[HTTP-DL][P%u/%u] header timeout\r\n", index + 1, k_parallel_count);
                    close_parallel_connection(connection);
                    --active_count;
                    continue;
                }
            } else {
                bool timed_out = false;
                const int ret = tls_recv(connection.transport, g_recv_buffer, sizeof(g_recv_buffer), timed_out);
                if (ret > 0) {
                    connection.total_bytes += static_cast<uint64_t>(ret);
                    connection.interval_bytes += static_cast<uint64_t>(ret);
                    made_progress = true;
                    if (connection.expected_bytes > 0 && connection.total_bytes >= connection.expected_bytes) {
                        connection.reached_eof = true;
                        print_parallel_summary(index + 1, connection);
                        close_parallel_connection(connection);
                        --active_count;
                        continue;
                    }
                } else if (ret == 0) {
                    connection.reached_eof = true;
                    print_parallel_summary(index + 1, connection);
                    close_parallel_connection(connection);
                    --active_count;
                    continue;
                } else if (!timed_out && ret != -2) {
                    osal_printk("[HTTP-DL][P%u/%u] body receive failed ret=%d errno=%d\r\n", index + 1,
                                k_parallel_count, ret, errno);
                    print_parallel_summary(index + 1, connection);
                    close_parallel_connection(connection);
                    --active_count;
                    continue;
                }

                const uint64_t report_ms = elapsed_ms(connection.report_start_ms);
                const uint64_t total_ms = elapsed_ms(connection.download_start_ms);
                if (report_ms >= 1000) {
                    const uint64_t interval_kbps = (report_ms > 0) ? (connection.interval_bytes * 8ULL / report_ms) : 0;
                    ++connection.report_count;
                    if (interval_kbps < 128) {
                        ++connection.low_rate_count;
                    }
                    if (connection.interval_bytes == 0) {
                        ++connection.zero_rate_count;
                    } else if (interval_kbps > 0 && interval_kbps < connection.min_nonzero_kbps) {
                        connection.min_nonzero_kbps = interval_kbps;
                    }
                    if (interval_kbps > connection.max_interval_kbps) {
                        connection.max_interval_kbps = interval_kbps;
                    }
                    print_parallel_rate(index + 1, connection, report_ms, total_ms);
                    connection.interval_bytes = 0;
                    connection.report_start_ms = uapi_systick_get_ms();
                }
            }
        }
        if (!made_progress) {
            osal_msleep(10);
        }
    }

    uint64_t aggregate_bytes = 0;
    unsigned int valid_count = 0;
    unsigned int eof_count = 0;
    for (unsigned int index = 0; index < k_parallel_count; ++index) {
        parallel_connection &connection = g_parallel_connections[index];
        if (connection.active) {
            print_parallel_summary(index + 1, connection);
            close_parallel_connection(connection);
        }
        if (connection.valid_response) {
            ++valid_count;
            aggregate_bytes += connection.total_bytes;
        }
        if (connection.reached_eof) {
            ++eof_count;
        }
    }

    const uint64_t wall_elapsed_ms = elapsed_ms(wall_start_ms);
    const uint64_t aggregate_kbps = (wall_elapsed_ms > 0) ? (aggregate_bytes * 8ULL / wall_elapsed_ms) : 0;
    osal_printk("[HTTP-DL] PARALLEL FINISHED valid=%u/%u eof=%u/%u total=%llu bytes wall=%llu ms aggregate=%llu kbps\r\n",
                valid_count, k_parallel_count, eof_count, k_parallel_count,
                static_cast<unsigned long long>(aggregate_bytes), static_cast<unsigned long long>(wall_elapsed_ms),
                static_cast<unsigned long long>(aggregate_kbps));
}
} // namespace

void *http_download_test_task(void *arg)
{
    (void)arg;
    osal_printk("[HTTP-DL] task started; target=http://%s:%u%s; connections=%u; max=%u ms\r\n",
                k_host, static_cast<unsigned>(k_port), k_path, k_parallel_count, k_download_timeout_ms);
    osal_printk("[HTTP-DL] waiting for STA connection and DHCP\r\n");
    if (!wait_for_wifi()) {
        return nullptr;
    }
    osal_printk("[HTTP-DL] WiFi ready, ip=%s\r\n", wifi_get_current_ip());
    run_parallel_tests();
    return nullptr;
}
