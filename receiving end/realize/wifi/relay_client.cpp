#include "relay_client.hpp"

extern "C" {
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "soc_osal.h"
}

#include "http_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr char k_fixed_play_url[] = "http://124.222.12.152:18080/ws63-test.mp3";
constexpr char k_relay_host[] = "124.222.12.152";
constexpr uint16_t k_relay_port = 18081;
constexpr char k_relay_path[] = "/relay/request";
constexpr char k_relay_token[] = "e495edb8873d12db703f936b72e2c823";

constexpr size_t k_max_source_url_length = 511;
constexpr size_t k_escaped_url_buffer_size = 1200;
constexpr size_t k_request_buffer_size = 2048;
constexpr size_t k_response_buffer_size = 512;

bool has_http_scheme(const char *url)
{
    if (url == nullptr) {
        return false;
    }

    return (strncmp(url, "http://", 7) == 0) || (strncmp(url, "https://", 8) == 0);
}

bool json_escape_url(const char *source, char *dest, size_t dest_size)
{
    if (source == nullptr || dest == nullptr || dest_size == 0) {
        return false;
    }

    size_t written = 0;
    for (size_t index = 0; source[index] != '\0'; ++index) {
        const unsigned char value = static_cast<unsigned char>(source[index]);
        if (value < 0x20U) {
            // A DLNA URL must not contain control characters. Rejecting them
            // avoids constructing malformed JSON and keeps the request bounded.
            return false;
        }

        const char *replacement = nullptr;
        if (value == '"') {
            replacement = "\\\"";
        } else if (value == '\\') {
            replacement = "\\\\";
        }

        if (replacement != nullptr) {
            const size_t replacement_length = strlen(replacement);
            if (written + replacement_length >= dest_size) {
                return false;
            }
            memcpy(dest + written, replacement, replacement_length);
            written += replacement_length;
        } else {
            if (written + 1 >= dest_size) {
                return false;
            }
            dest[written++] = static_cast<char>(value);
        }
    }

    dest[written] = '\0';
    return true;
}

bool send_all(int sock, const char *data, size_t length)
{
    if (sock < 0 || data == nullptr || length == 0) {
        return false;
    }

    size_t sent_total = 0;
    while (sent_total < length) {
        const int sent = lwip_send(sock, data + sent_total, static_cast<int>(length - sent_total), 0);
        if (sent <= 0) {
            return false;
        }
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

int parse_http_status_code(const char *response)
{
    if (response == nullptr || strncmp(response, "HTTP/", 5) != 0) {
        return -1;
    }

    const char *space = strchr(response, ' ');
    if (space == nullptr || space[1] < '0' || space[1] > '9' || space[2] < '0' || space[2] > '9' ||
        space[3] < '0' || space[3] > '9') {
        return -1;
    }

    return (space[1] - '0') * 100 + (space[2] - '0') * 10 + (space[3] - '0');
}

} // namespace

namespace ws63_relay {

const char *fixed_play_url()
{
    return k_fixed_play_url;
}

bool submit_source_url(const char *source_url)
{
    if (source_url == nullptr || source_url[0] == '\0' || strlen(source_url) > k_max_source_url_length ||
        !has_http_scheme(source_url)) {
        osal_printk("[Relay] invalid source URL\r\n");
        return false;
    }

    std::array<char, k_escaped_url_buffer_size> escaped_url = {0};
    if (!json_escape_url(source_url, escaped_url.data(), escaped_url.size())) {
        osal_printk("[Relay] source URL JSON encoding failed\r\n");
        return false;
    }

    std::array<char, k_request_buffer_size> request = {0};
    const int request_length = snprintf(
        request.data(), request.size(),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "X-Relay-Token: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n"
        "{\"url\":\"%s\"}",
        k_relay_path, k_relay_host, static_cast<unsigned int>(k_relay_port), k_relay_token,
        static_cast<unsigned int>(strlen(escaped_url.data()) + 10U), escaped_url.data());
    if (request_length <= 0 || request_length >= static_cast<int>(request.size())) {
        osal_printk("[Relay] request too large\r\n");
        return false;
    }

    const int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        osal_printk("[Relay] socket create failed\r\n");
        return false;
    }

    timeval timeout = {5, 0};
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port = lwip_htons(k_relay_port);
    if (!resolve_ipv4_addr(k_relay_host, &server.sin_addr)) {
        osal_printk("[Relay] server address resolve failed\r\n");
        lwip_close(sock);
        return false;
    }

    if (lwip_connect(sock, reinterpret_cast<sockaddr *>(&server), sizeof(server)) < 0) {
        osal_printk("[Relay] connect failed\r\n");
        lwip_close(sock);
        return false;
    }

    if (!send_all(sock, request.data(), static_cast<size_t>(request_length))) {
        osal_printk("[Relay] request send failed\r\n");
        lwip_close(sock);
        return false;
    }

    std::array<char, k_response_buffer_size> response = {0};
    size_t received = 0;
    while (received < response.size() - 1U) {
        const int count = lwip_recv(sock, response.data() + received,
                                    static_cast<int>(response.size() - 1U - received), 0);
        if (count <= 0) {
            break;
        }
        received += static_cast<size_t>(count);
        response[received] = '\0';
        if (strstr(response.data(), "\r\n") != nullptr) {
            break;
        }
    }

    const int status_code = parse_http_status_code(response.data());
    lwip_close(sock);
    if (status_code != 202) {
        osal_printk("[Relay] request rejected, HTTP status=%d\r\n", status_code);
        return false;
    }

    osal_printk("[Relay] source accepted, download started\r\n");
    return true;
}

} // namespace ws63_relay
