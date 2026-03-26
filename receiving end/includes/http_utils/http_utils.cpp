#include "http_utils.hpp"

void copy_string_safe(char *dst, size_t dst_size, const char *src)
{
    if (dst == nullptr || dst_size == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

void html_entity_decode_amp(char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    char *read_p = text;
    char *write_p = text;
    while (*read_p != '\0') {
        if (strncmp(read_p, "&amp;", 5) == 0) {
            *write_p++ = '&';
            read_p += 5;
            continue;
        }
        *write_p++ = *read_p++;
    }
    *write_p = '\0';
}

void strip_angle_brackets(char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    trim_ascii_whitespace(text);
    size_t len = strlen(text);
    if (len >= 2 && text[0] == '<' && text[len - 1] == '>') {
        memmove(text, text + 1, len - 2);
        text[len - 2] = '\0';
    }
}

bool parse_http_url(const char *url, simple_http_url &out)
{
    if (url == nullptr) {
        return false;
    }

    const char *p = url;
    if (strncmp(p, "http://", 7) != 0) {
        return false;
    }
    p += 7;

    const char *path_start = strchr(p, '/');
    const char *host_end = (path_start != nullptr) ? path_start : (p + strlen(p));
    const char *port_sep = nullptr;
    for (const char *it = p; it < host_end; ++it) {
        if (*it == ':') {
            port_sep = it;
        }
    }

    size_t host_len = (port_sep != nullptr) ? static_cast<size_t>(port_sep - p) : static_cast<size_t>(host_end - p);
    if (host_len == 0 || host_len >= out.host.size()) {
        return false;
    }
    memcpy(out.host.data(), p, host_len);
    out.host[host_len] = '\0';

    if (port_sep != nullptr) {
        int port = atoi(port_sep + 1);
        if (port <= 0 || port > 65535) {
            return false;
        }
        out.port = static_cast<uint16_t>(port);
    } else {
        out.port = 80;
    }

    if (path_start != nullptr) {
        copy_string_safe(out.path.data(), out.path.size(), path_start);
    } else {
        copy_string_safe(out.path.data(), out.path.size(), "/");
    }

    return true;
}

bool resolve_ipv4_addr(const char *host, in_addr *out_addr)
{
    if (host == nullptr || out_addr == nullptr) {
        return false;
    }

    // Fast path: dotted-decimal IPv4 literal.
    if (inet_aton(host, out_addr) != 0) {
        return true;
    }

    // Fallback: resolve DNS host name via lwIP netdb API.
    hostent *entry = lwip_gethostbyname(host);
    if (entry == nullptr || entry->h_addr_list == nullptr || entry->h_addr_list[0] == nullptr) {
        return false;
    }
    if (entry->h_addrtype != AF_INET || entry->h_length < static_cast<int>(sizeof(in_addr))) {
        return false;
    }

    memcpy(out_addr, entry->h_addr_list[0], sizeof(in_addr));
    return true;
}

bool extract_http_header_value(const char *request, const char *key, char *out, size_t out_size)
{
    if (request == nullptr || key == nullptr || out == nullptr || out_size == 0) {
        return false;
    }

    const size_t key_len = strlen(key);
    const char *line = request;
    while (*line != '\0') {
        const char *line_end = strstr(line, "\r\n");
        if (line_end == nullptr) {
            line_end = line + strlen(line);
        }

        size_t i = 0;
        while (i < key_len && (line + i) < line_end) {
            char a = static_cast<char>(tolower(static_cast<unsigned char>(line[i])));
            char b = static_cast<char>(tolower(static_cast<unsigned char>(key[i])));
            if (a != b) {
                break;
            }
            ++i;
        }

        if (i == key_len && (line + i) < line_end && line[i] == ':') {
            const char *val = line + i + 1;
            while (val < line_end && (*val == ' ' || *val == '\t')) {
                ++val;
            }

            size_t copy_len = static_cast<size_t>(line_end - val);
            if (copy_len >= out_size) {
                copy_len = out_size - 1;
            }
            memcpy(out, val, copy_len);
            out[copy_len] = '\0';
            return true;
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
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    size_t start = 0;
    size_t end = strlen(text);

    while (start < end && isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    while (end > start && isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }

    if (start > 0) {
        memmove(text, text + start, end - start);
    }
    text[end - start] = '\0';
}

bool ascii_iequals(const char *a, const char *b)
{
    if (a == nullptr || b == nullptr) {
        return false;
    }

    while (*a != '\0' && *b != '\0') {
        char ca = static_cast<char>(tolower(static_cast<unsigned char>(*a)));
        char cb = static_cast<char>(tolower(static_cast<unsigned char>(*b)));
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }

    return (*a == '\0' && *b == '\0');
}

bool ascii_icontains(const char *haystack, const char *needle)
{
    if (haystack == nullptr || needle == nullptr || needle[0] == '\0') {
        return false;
    }

    const size_t needle_len = strlen(needle);
    for (size_t i = 0; haystack[i] != '\0'; ++i) {
        size_t j = 0;
        while (j < needle_len && haystack[i + j] != '\0') {
            char ch = static_cast<char>(tolower(static_cast<unsigned char>(haystack[i + j])));
            char cn = static_cast<char>(tolower(static_cast<unsigned char>(needle[j])));
            if (ch != cn) {
                break;
            }
            ++j;
        }
        if (j == needle_len) {
            return true;
        }
    }
    return false;
}

char *extract_xml_tag_value(const char *buffer, const char *tag, char *out, size_t out_size)
{
    if (buffer == nullptr || tag == nullptr || out == nullptr || out_size == 0) {
        return nullptr;
    }

    char start_tag[256] = {0};
    char end_tag[256] = {0};
    snprintf(start_tag, sizeof(start_tag), "<%s>", tag);
    snprintf(end_tag, sizeof(end_tag), "</%s>", tag);

    const char *tag_start = strstr(buffer, start_tag);
    if (tag_start == nullptr) {
        return nullptr;
    }

    tag_start += strlen(start_tag);
    const char *tag_end = strstr(tag_start, end_tag);
    if (tag_end == nullptr) {
        return nullptr;
    }

    int value_len = tag_end - tag_start;
    if (value_len > 0 && value_len < (int)out_size - 1) {
        strncpy(out, tag_start, value_len);
        out[value_len] = '\0';
        return out;
    }

    return nullptr;
}
