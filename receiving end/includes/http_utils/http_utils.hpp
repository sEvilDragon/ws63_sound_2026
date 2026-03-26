#ifndef __HTTP_UTILS_HPP__
#define __HTTP_UTILS_HPP__

extern "C" {
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/inet.h"
#include "soc_osal.h"
}

#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>

struct simple_http_url {
    std::array<char, 128> host = {0};
    std::array<char, 512> path = {0};
    uint16_t port = 80;
};

void copy_string_safe(char *dst, size_t dst_size, const char *src);
void html_entity_decode_amp(char *text);
void strip_angle_brackets(char *text);
bool parse_http_url(const char *url, simple_http_url &out);
bool resolve_ipv4_addr(const char *host, in_addr *out_addr);
bool extract_http_header_value(const char *request, const char *key, char *out, size_t out_size);
void trim_ascii_whitespace(char *text);
bool ascii_iequals(const char *a, const char *b);
bool ascii_icontains(const char *haystack, const char *needle);
char *extract_xml_tag_value(const char *buffer, const char *tag, char *out, size_t out_size);

#endif