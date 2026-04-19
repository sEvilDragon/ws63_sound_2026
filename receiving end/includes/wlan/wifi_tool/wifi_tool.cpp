#include "wifi_tool.hpp"

namespace sed_ws63 {

errcode_t wifi_tool::copy_str(char *dest, size_t dest_size, const char *src, size_t src_size)
{
    if (dest == nullptr || src == nullptr) {
        return 0x01; // 参数错误
    }
    if (dest_size == 0) {
        return 0x02; // 目标缓冲区大小不足
    }

    size_t src_len = src_size == 0 ? strlen(src) : src_size;
    if (src_len >= dest_size) {
        return 0x02; // 目标缓冲区大小不足
    }
    memcpy(dest, src, src_len);
    dest[src_len] = '\0';
    return ERRCODE_SUCC;
}

errcode_t wifi_tool::trim(char *text)
{
    if (text == nullptr) {
        return 0x01; // 参数错误
    }
    size_t len = 0;
    while (text[len] != '\0') {
        len++;
    }
    size_t start = 0;
    while (start < len) {
        char ch = text[start];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v') {
            break;
        }
        start++;
    }
    size_t end = len;
    while (end > start) {
        char ch = text[end - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v') {
            break;
        }
        end--;
    }
    if (start > 0) {
        memmove(text, text + start, end - start);
    }
    text[end - start] = '\0';
    return ERRCODE_SUCC;
}

errcode_t wifi_tool::strip(char *text, char left, char right)
{
    if (text == nullptr) {
        return 0x01; // 参数错误
    }
    size_t len = strlen(text);
    if (len < 2) {
        return 0x01; // 参数错误
    }
    if (text[0] != left || text[len - 1] != right) {
        return 0x03; // 没有发现需要去除的字符
    }
    memmove(text, text + 1, len - 2);
    text[len - 2] = '\0';

    return ERRCODE_SUCC;
}

errcode_t wifi_tool::trim_and_strip(char *text, char left, char right)
{
    errcode_t trim_result = trim(text);
    if (trim_result != ERRCODE_SUCC) {
        return trim_result;
    }
    return strip(text, left, right);
}

char wifi_tool::to_lower(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch + ('a' - 'A');
    }
    return ch;
}

bool wifi_tool::strcmp_ignore_case(const char *text1, const char *text2)
{
    if (text1 == nullptr || text2 == nullptr) {
        return false;
    }

    while (*text1 != '\0' && *text2 != '\0') {
        if (to_lower(*text1) != to_lower(*text2)) {
            return false;
        }
        text1++;
        text2++;
    }
    return *text1 == '\0' && *text2 == '\0';
}

const char *wifi_tool::strstr_s(const char *text, const char *substr)
{
    if (text == nullptr || substr == nullptr || *substr == '\0') {
        return nullptr;
    }

    for (size_t i = 0; text[i] != '\0'; i++) {
        size_t j = 0;
        while (substr[j] != '\0' && text[i + j] != '\0' && text[i + j] == substr[j]) {
            j++;
        }
        if (substr[j] == '\0') {
            return &text[i];
        }
    }
    return nullptr;
}

const char *wifi_tool::strstr_ignore_case(const char *text, const char *substr)
{
    if (text == nullptr || substr == nullptr || *substr == '\0') {
        return nullptr;
    }

    for (size_t i = 0; text[i] != '\0'; i++) {
        size_t j = 0;
        while (substr[j] != '\0' && text[i + j] != '\0' && to_lower(text[i + j]) == to_lower(substr[j])) {
            j++;
        }
        if (substr[j] == '\0') {
            return &text[i];
        }
    }
    return nullptr;
}

bool wifi_tool::is_strstr_s(const char *text, const char *substr)
{
    return strstr_s(text, substr) != nullptr;
}

bool wifi_tool::is_strstr_ignore_case(const char *text, const char *substr)
{
    return strstr_ignore_case(text, substr) != nullptr;
}

const wifi_tool::span_text wifi_tool::find_html_tag_value(const char *text, const char *tag)
{
    if (text == nullptr || tag == nullptr || *text == '\0' || *tag == '\0') {
        return {nullptr, 0};
    }

    // 拼出完整标签：<tag>
    char start_tag[64] = {0};
    snprintf(start_tag, sizeof(start_tag), "<%s>", tag);

    // 拼出结束标签：</tag>
    char end_tag[64] = {0};
    snprintf(end_tag, sizeof(end_tag), "</%s>", tag);

    // 找到开始标签
    const char *value_start = strstr_s(text, start_tag);
    if (value_start == nullptr) {
        return {nullptr, 0};
    }
    value_start += strlen(start_tag);

    // 找到结束标签
    const char *value_end = strstr_s(value_start, end_tag);
    if (value_end == nullptr) {
        return {nullptr, 0};
    }

    size_t value_length = static_cast<size_t>(value_end - value_start);
    return {value_start, value_length};
}

const wifi_tool::span_text wifi_tool::find_http_header_value(const char *text, const char *key)
{
    if (text == nullptr || key == nullptr || *text == '\0' || *key == '\0') {
        return {nullptr, 0};
    }

    const char *line_start = text;
    while (*line_start != '\0') {
        const char *line_end = strstr_s(line_start, "\r\n");
        if (line_end == nullptr) {
            line_end = line_start + strlen(line_start);
        }

        // 查找冒号分隔符
        const char *colon_pos = strstr_s(line_start, ":");
        if (colon_pos != nullptr && colon_pos < line_end) {
            // 提取键和值
            size_t key_len = colon_pos - line_start;
            if (key_len == strlen(key) && strcmp_ignore_case(line_start, key)) {
                // 找到匹配的键，提取值
                const char *value_start = colon_pos + 1;
                while (*value_start == ' ' || *value_start == '\t') {
                    value_start++;
                }
                size_t value_length = static_cast<size_t>(line_end - value_start);
                return {value_start, value_length};
            }
        }

        if (*line_end == '\0') {
            break;
        }
        line_start = line_end + 2; // 跳过\r\n
    }
    return {nullptr, 0};
}

const wifi_tool::span_text wifi_tool::trim_and_find_tag_value(char *text, const char *tag)
{
    trim(text); // 去除前后空白字符
    return find_html_tag_value(text, tag);
}

const wifi_tool::span_text wifi_tool::trim_and_find_http_header_value(char *text, const char *key)
{
    trim(text); // 去除前后空白字符
    return find_http_header_value(text, key);
}

errcode_t wifi_tool::decode_xml_basic(char *text)
{
    if (text == nullptr) {
        return 0x01; // 参数错误
    }

    char *read_p = text;
    char *write_p = text;
    size_t replaced = 0;

    while (*read_p != '\0') {
        if (strncmp(read_p, "&amp;", 5) == 0) {
            *write_p++ = '&';
            read_p += 5;
            replaced++;
        } else if (strncmp(read_p, "&lt;", 4) == 0) {
            *write_p++ = '<';
            read_p += 4;
            replaced++;
        } else if (strncmp(read_p, "&gt;", 4) == 0) {
            *write_p++ = '>';
            read_p += 4;
            replaced++;
        } else if (strncmp(read_p, "&quot;", 6) == 0) {
            *write_p++ = '"';
            read_p += 6;
            replaced++;
        } else if (strncmp(read_p, "&apos;", 6) == 0) {
            *write_p++ = '\'';
            read_p += 6;
            replaced++;
        } else {
            *write_p++ = *read_p++;
        }
    }
    *write_p = '\0';

    if (replaced == 0) {
        return 0x03; // 没有任何实体被解码
    }

    return ERRCODE_SUCC;
}

errcode_t wifi_tool::strict_atoi(const char *text, auto &answer)
{
    if (text == nullptr || *text == '\0') {
        return 0x01; // 参数错误
    }

    uint64_t result = 0;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        char ch = text[i];
        if (ch < '0' || ch > '9') {
            return 0x04; // 字符串格式错误
        }
        int digit = ch - '0';
        result = result * 10 + digit;
        if (result > static_cast<uint64_t>(std::numeric_limits<decltype(answer)>::max())) {
            return 0x05; // 数字超出范围
        }
    }

    answer = result;
    return ERRCODE_SUCC;
}

errcode_t wifi_tool::get_url(const char *url, parse_url &result)
{
    if (url == nullptr || strlen(url) <= 7) {
        return 0x01; // 参数错误
    }
    const char *scheme_end = strstr_s(url, "://");
    if (url[6] == '/' && scheme_end == nullptr) {
        return 0x06; // 非法URL格式
    }

    // 解析scheme
    if (scheme_end - url == 4 && strncmp(url, "http", 4) == 0) {
        strncpy(result.scheme.data(), "http", result.scheme.size() - 1);
    } else if (scheme_end - url == 5 && strncmp(url, "https", 5) == 0) {
        strncpy(result.scheme.data(), "https", result.scheme.size() - 1);
    } else {
        return 0x06; // 非法URL格式
    }

    // 解析host
    const char *host_start = scheme_end + 3;
    const char *path_start = strchr(host_start, '/');
    const char *host_end = path_start == nullptr ? (url + strlen(url)) : path_start;

    // 注意，只提供ipv4的支持
    const char *port_sep = nullptr;
    for (const char *it = host_start; it < host_end; it++) {
        if (*it == ':') {
            port_sep = it;
            break;
        }
    }

    size_t host_length =
        (port_sep != nullptr) ? static_cast<size_t>(port_sep - host_start) : static_cast<size_t>(host_end - host_start);
    if (host_length == 0) {
        return 0x07; // 缺失host
    }
    if (host_length >= result.host.size()) {
        return 0x08; // host长度超过限制
    }
    copy_str(result.host.data(), result.host.size(), host_start, host_length);

    // 解析port
    if (port_sep != nullptr) {
        char port_text[16] = {0};
        size_t port_length = static_cast<size_t>(host_end - port_sep - 1);
        if (port_length == 0) {
            return 0x09; // port缺失
        }
        if (port_length >= sizeof(port_text)) {
            return 0x0A; // port长度超过限制
        }
        copy_str(port_text, sizeof(port_text), port_sep + 1, port_length);
        uint16_t port = 0;
        errcode_t port_parse_result = strict_atoi(port_text, port);
        if (port_parse_result != ERRCODE_SUCC) {
            return 0x0B; // port格式错误
        }
        result.port = port;
        result.has_explicit_port = true;
        copy_str(result.port_text.data(), result.port_text.size(), port_text, port_length);
    } else {
        // 没有指定端口，使用默认端口
        if (strcmp(result.scheme.data(), "http") == 0) {
            result.port = 80;
        } else if (strcmp(result.scheme.data(), "https") == 0) {
            result.port = 443;
        }
        result.has_explicit_port = false;
        result.port_text[0] = '\0';
    }

    // 解析路径
    const char *path = path_start != nullptr ? path_start : "/";
    if (strlen(path) >= result.path.size()) {
        return 0x0C; // 路径长度超过限制
    }
    copy_str(result.path.data(), result.path.size(), path);
    return ERRCODE_SUCC;
}

errcode_t wifi_tool::split_url_path(parse_url &url_info)
{
    if (url_info.path[0] == '\0') {
        return 0x01; // 参数错误
    }
    const char *path_start = url_info.path.data();
    const char *query_start = strchr(url_info.path.data(), '?');
    const char *fragment_start = strchr(url_info.path.data(), '#');
    if (query_start == nullptr && fragment_start == nullptr) {
        return 0x0D; // URL格式错误，不存在路径部分或者不存在所需的分隔符
    }

    // 重写路径部分
    memset(url_info.path.data(), 0, url_info.path.size());
    size_t path_len = query_start - path_start;
    if (path_len >= url_info.path.size()) {
        return 0x0C; // 路径长度超过限制
    }
    copy_str(url_info.path.data(), url_info.path.size(), path_start, path_len);

    // 提取query部分
    size_t query_len = fragment_start - query_start - 1;
    if (query_len >= url_info.query.size()) {
        return 0x0E; // query长度超过限制
    }
    copy_str(url_info.query.data(), url_info.query.size(), query_start + 1, query_len);

    // 提取fragment部分
    size_t fragment_len = strlen(fragment_start + 1);
    if (fragment_len >= url_info.fragment.size()) {
        return 0x0F; // fragment长度超过限制
    }
    copy_str(url_info.fragment.data(), url_info.fragment.size(), fragment_start + 1, fragment_len);

    return ERRCODE_SUCC;
}

errcode_t wifi_tool::get_ipv4_addr(const char *host, in_addr *out_addr)
{
    if (host == nullptr || *host == '\0' || out_addr == nullptr) {
        return 0x01; // 参数错误
    }

    if (inet_aton(host, out_addr) != 0) {
        // 说明获得的就是地址
        return ERRCODE_SUCC; // 解析成功
    }
    // 获得的是域名，进行解析
    hostent *entry = lwip_gethostbyname(host);

    if (entry == nullptr || entry->h_addr_list == nullptr || entry->h_addr_list[0] == nullptr) {
        return 0x10; // 无法解析地址
    }
    if (entry->h_addrtype != AF_INET || entry->h_length < static_cast<int>(sizeof(in_addr))) {
        return 0x11; // 解析结果不是IPv4地址
    }

    memcpy(out_addr, entry->h_addr_list[0], sizeof(in_addr));
    return ERRCODE_SUCC;
}
} // namespace sed_ws63