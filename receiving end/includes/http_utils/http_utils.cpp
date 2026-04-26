#include "http_utils.hpp"
#include "wifi_tool.hpp"

using wifi_tool_t = sed_ws63::wifi_tool;

namespace {

bool copy_span_compat(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    if (dst == nullptr || dst_size == 0) {
        return false;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return false;
    }
    if (src_len == 0) {
        dst[0] = '\0';
        return true;
    }

    size_t copy_len = src_len;
    if (copy_len >= dst_size) {
        copy_len = dst_size - 1;
    }

    return wifi_tool_t::copy_str(dst, dst_size, src, copy_len) == ERRCODE_SUCC;
}

bool copy_cstr_compat(char *dst, size_t dst_size, const char *src)
{
    if (src == nullptr) {
        if (dst != nullptr && dst_size > 0) {
            dst[0] = '\0';
        }
        return false;
    }

    return copy_span_compat(dst, dst_size, src, strlen(src));
}

bool ascii_equals_span_ignore_case(const char *text, size_t text_len, const char *expected)
{
    if (text == nullptr || expected == nullptr) {
        return false;
    }

    const size_t expected_len = strlen(expected);
    if (text_len != expected_len) {
        return false;
    }

    for (size_t i = 0; i < text_len; ++i) {
        if (wifi_tool_t::to_lower(text[i]) != wifi_tool_t::to_lower(expected[i])) {
            return false;
        }
    }

    return true;
}

} // namespace

void copy_string_safe(char *dst, size_t dst_size, const char *src)
{
    (void)copy_cstr_compat(dst, dst_size, src);
}

void html_entity_decode_amp(char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    (void)wifi_tool_t::decode_xml_basic(text);
}

void strip_angle_brackets(char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    (void)wifi_tool_t::trim_and_strip(text, '<', '>');
}

bool parse_http_url(const char *url, simple_http_url &out)
{
    if (url == nullptr || strncmp(url, "http://", 7) != 0) {
        return false;
    }

    wifi_tool_t::parse_url parsed = {};
    if (wifi_tool_t::get_url(url, parsed) != ERRCODE_SUCC) {
        return false;
    }

    if (!wifi_tool_t::strcmp_ignore_case(parsed.scheme.data(), "http")) {
        return false;
    }

    if (!copy_cstr_compat(out.host.data(), out.host.size(), parsed.host.data())) {
        return false;
    }
    if (!copy_cstr_compat(out.path.data(), out.path.size(), parsed.path.data())) {
        return false;
    }

    out.port = parsed.port;
    return true;
}

bool resolve_ipv4_addr(const char *host, in_addr *out_addr)
{
    return wifi_tool_t::get_ipv4_addr(host, out_addr) == ERRCODE_SUCC;
}

bool extract_http_header_value(const char *request, const char *key, char *out, size_t out_size)
{
    if (request == nullptr || key == nullptr || out == nullptr || out_size == 0) {
        return false;
    }

    const char *line = request;
    while (*line != '\0') {
        const char *line_end = wifi_tool_t::strstr_s(line, "\r\n");
        if (line_end == nullptr) {
            line_end = line + strlen(line);
        }

        const char *colon_pos = wifi_tool_t::strstr_s(line, ":");
        if (colon_pos != nullptr && colon_pos < line_end) {
            const size_t field_len = static_cast<size_t>(colon_pos - line);
            if (ascii_equals_span_ignore_case(line, field_len, key)) {
                const char *value = colon_pos + 1;
                while (value < line_end && (*value == ' ' || *value == '\t')) {
                    ++value;
                }

                return copy_span_compat(out, out_size, value, static_cast<size_t>(line_end - value));
            }
        }

        if (*line_end == '\0') {
            break;
        }
        line = line_end + 2;
    }

    return false;
}

void trim_ascii_whitespace(char *text)
{
    (void)wifi_tool_t::trim(text);
}

bool ascii_iequals(const char *a, const char *b)
{
    return wifi_tool_t::strcmp_ignore_case(a, b);
}

bool ascii_icontains(const char *haystack, const char *needle)
{
    return wifi_tool_t::is_strstr_ignore_case(haystack, needle);
}

char *extract_xml_tag_value(const char *buffer, const char *tag, char *out, size_t out_size)
{
    if (buffer == nullptr || tag == nullptr || out == nullptr || out_size == 0) {
        return nullptr;
    }

    const wifi_tool_t::span_text value = wifi_tool_t::find_html_tag_value(buffer, tag);
    if (value.ptr == nullptr) {
        return nullptr;
    }

    if (value.len == 0 || value.len >= out_size - 1) {
        return nullptr;
    }

    if (copy_span_compat(out, out_size, value.ptr, value.len)) {
        return out;
    }

    return nullptr;
}
