#include "dlan.hpp"
dlan::dlan() {}

// 初始化静态成员变量
std::array<char, 16> dlan::local_ip = {0};
bool dlan::is_ready = false;
int32_t dlan::ssdp_sock = -1;
int32_t dlan::http_sock = -1;
dlan::data_process_t dlan::data_process = nullptr;
dlan::data_clear_t dlan::data_clear = nullptr;

namespace {
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
} // namespace

bool dlan::ssdp_get_header_value(const char *request, const char *key, char *out, size_t out_size)
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

bool dlan::ssdp_send_msearch_reply(const sockaddr_in &client_addr,
                                   socklen_t client_addr_len,
                                   const char *st,
                                   const char *usn_suffix)
{
    if (st == nullptr || st[0] == '\0') {
        return false;
    }

    std::array<char, 256> usn = {0};
    if (usn_suffix != nullptr && usn_suffix[0] != '\0') {
        snprintf(usn.data(), usn.size(), "%s::%s", ssdp_uuid.data(), usn_suffix);
    } else {
        snprintf(usn.data(), usn.size(), "%s", ssdp_uuid.data());
    }

    std::array<char, 896> response = {0};
    snprintf(response.data(), response.size(),
             "HTTP/1.1 200 OK\r\n"
             "CACHE-CONTROL: max-age=%d\r\n"
             "EXT:\r\n"
             "LOCATION: http://%s:%u/description.xml\r\n"
             "SERVER: Linux/5.10 UPnP/1.1 WS63/1.0\r\n"
             "ST: %s\r\n"
             "USN: %s\r\n"
             "BOOTID.UPNP.ORG: %d\r\n"
             "CONFIGID.UPNP.ORG: %d\r\n"
             "\r\n",
             ssdp_timeout, local_ip.data(), http_port, st, usn.data(), ssdp_boot_id, ssdp_config_id);

    int32_t send_ret =
        sendto(ssdp_sock, response.data(), strlen(response.data()), 0, (const sockaddr *)&client_addr, client_addr_len);
    if (send_ret < 0) {
        osal_printk("ssdp响应发送失败(ST=%s)\n", st);
        return false;
    }

    osal_printk("ssdp已响应M-SEARCH (ST=%s, USN=%s)\n", st, usn.data());
    return true;
}

void dlan::ssdp_and_http_scan()
{
    while (!is_ready) {
        // 等待网络准备就绪，获取本地IP地址等信息
        osal_msleep(100);
    }
    // WiFi连接成功并不代表DHCP已完成，等待拿到有效IP再启动DLNA。
    while (true) {
        ssdp_ip_get();
        if (strcmp(local_ip.data(), "0.0.0.0") != 0) {
            break;
        }
        osal_printk("dlan等待有效IP中...\n");
        osal_msleep(500);
    }
    // 建立SSDP（UDP）套接字
    ssdp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ssdp_sock < 0) {
        osal_printk("ssdp进程启动失败\n");
        return;
    }
    if (!ssdp_set()) {
        dlan_stop();
        return;
    }

    // 建立HTTP（TCP）套接字
    http_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (http_sock < 0) {
        osal_printk("http进程启动失败\n");
        dlan_stop();
        return;
    }
    if (!http_set()) {
        dlan_stop();
        return;
    }

    // SED : 串口输出
    osal_printk("dlan启动成功，local_ip=%s, ssdp=%d, http=%d\n", local_ip.data(), ssdp_sock, http_sock);

    fd_set read_fds;
    memset(&read_fds, 0, sizeof(read_fds));
    int max_fd = (ssdp_sock > http_sock) ? ssdp_sock : http_sock;

    while (true) {
        memset(&read_fds, 0, sizeof(read_fds));
        FD_SET(ssdp_sock, &read_fds); // 把 UDP 监控加进去
        FD_SET(http_sock, &read_fds); // 把 TCP 监控加进去

        int ret = lwip_select(max_fd + 1, &read_fds, nullptr, nullptr, nullptr);

        if (ret < 0) {
            osal_printk("select出错\n");
            continue;
        }

        if (FD_ISSET(ssdp_sock, &read_fds)) {
            ssdp_process();
        }
        if (FD_ISSET(http_sock, &read_fds)) {
            // SED : 串口输出
            osal_printk("select命中http fd\n");
            http_process();
        }
    }
}

bool dlan::ssdp_set()
{
    int32_t sock = ssdp_sock;
    int32_t reuse = 1;

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        osal_printk("ssdp套接字设置SO_REUSEADDR失败\n");
    }

    // 配置SSDP套接字
    sockaddr_in ssdp_addr = {0};
    ssdp_addr.sin_family = AF_INET;
    ssdp_addr.sin_addr.s_addr = INADDR_ANY;
    ssdp_addr.sin_port = htons(ssdp_port);

    errcode_t ret = bind(sock, (sockaddr *)&ssdp_addr, sizeof(ssdp_addr));
    if (ret != 0) {
        osal_printk("ssdp套接字绑定失败\n");
        lwip_close(sock);
        ssdp_sock = -1;
        return false;
    }

    // 加入多播组
    ip_mreq mreq = {0};
    mreq.imr_multiaddr.s_addr = inet_addr(mcast_ip.data());
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        osal_printk("ssdp套接字加入多播组失败\n");
        lwip_close(sock);
        ssdp_sock = -1;
        return false;
    }

    return true;
}

bool dlan::http_set()
{
    int32_t sock = http_sock;
    int32_t reuse = 1;

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        osal_printk("http套接字设置SO_REUSEADDR失败\n");
    }

    // 配置HTTP套接字
    sockaddr_in http_addr = {0};
    http_addr.sin_family = AF_INET;
    http_addr.sin_addr.s_addr = INADDR_ANY;
    http_addr.sin_port = htons(http_port);
    errcode_t ret = bind(sock, (sockaddr *)&http_addr, sizeof(http_addr));
    if (ret != 0) {
        osal_printk("http套接字绑定失败\n");
        lwip_close(sock);
        http_sock = -1;
        return false;
    }

    // 调用listen函数监听HTTP套接字，设置最大连接数为2
    ret = listen(sock, 2);
    if (ret != 0) {
        osal_printk("http套接字监听失败\n");
        lwip_close(sock);
        http_sock = -1;
        return false;
    }

    // SED : 串口输出
    osal_printk("http监听已启动，端口=%u\n", http_port);

    return true;
}

void dlan::ssdp_process()
{
    int32_t sock = ssdp_sock;
    static std::array<char, 1024> buffer; // SSDP接收缓冲区
    sockaddr_in client_addr = {0};        // 记录SSDP消息来源
    socklen_t client_addr_len = sizeof(client_addr);

    int ret = recvfrom(sock, buffer.data(), buffer.size() - 1, 0, (sockaddr *)&client_addr, &client_addr_len);

    if (ret < 0) {
        osal_printk("ssdp数据接收失败\n");
        return;
    }

    buffer[ret] = '\0';

    // 处理 M-SEARCH 搜索报文
    bool is_msearch = strstr(buffer.data(), "M-SEARCH") != nullptr;
    // 处理 NOTIFY 广播通知（我们只需要忽略它，不当作错误）
    bool is_notify = strstr(buffer.data(), "NOTIFY") != nullptr;

    if (is_msearch) {
        std::array<char, 128> st_value = {0};
        std::array<char, 128> man_value = {0};
        std::array<char, 64> host_value = {0};
        bool has_st = ssdp_get_header_value(buffer.data(), "ST", st_value.data(), st_value.size());
        bool has_man = ssdp_get_header_value(buffer.data(), "MAN", man_value.data(), man_value.size());
        bool has_host = ssdp_get_header_value(buffer.data(), "HOST", host_value.data(), host_value.size());
        if (has_st) {
            trim_ascii_whitespace(st_value.data());
        }
        if (has_man) {
            trim_ascii_whitespace(man_value.data());
        }
        if (has_host) {
            trim_ascii_whitespace(host_value.data());
        }

        if (!has_st || !has_man || !ascii_icontains(man_value.data(), "ssdp:discover")) {
            osal_printk("ssdp忽略M-SEARCH: MAN/ST不合法, MAN=%s, ST=%s\n", man_value.data(), st_value.data());
            return;
        }

        if (has_host && strstr(host_value.data(), "239.255.255.250:1900") == nullptr) {
            osal_printk("ssdp忽略M-SEARCH: HOST=%s\n", host_value.data());
            return;
        }

        osal_printk("ssdp收到M-SEARCH: ST=%s\n", st_value.data());

        const bool is_ssdp_all = ascii_iequals(st_value.data(), "ssdp:all");
        const bool is_root = ascii_iequals(st_value.data(), "upnp:rootdevice");
        const bool is_uuid = ascii_iequals(st_value.data(), ssdp_uuid.data());
        const bool is_renderer = ascii_iequals(st_value.data(), "urn:schemas-upnp-org:device:MediaRenderer:1");
        const bool is_av_transport = ascii_iequals(st_value.data(), "urn:schemas-upnp-org:service:AVTransport:1");
        const bool is_rendering_control =
            ascii_iequals(st_value.data(), "urn:schemas-upnp-org:service:RenderingControl:1");
        const bool is_connection_manager =
            ascii_iequals(st_value.data(), "urn:schemas-upnp-org:service:ConnectionManager:1");
        const bool is_qplay = strstr(st_value.data(), "QPlay") != nullptr;

        if (!(is_ssdp_all || is_root || is_uuid || is_renderer || is_av_transport || is_rendering_control ||
              is_connection_manager || is_qplay)) {
            osal_printk("ssdp忽略M-SEARCH: ST=%s\n", st_value.data());
            return;
        }

        if (is_ssdp_all) {
            ssdp_send_msearch_reply(client_addr, client_addr_len, "UPnP:rootdevice", "UPnP:rootdevice");
            osal_msleep(20);
            ssdp_send_msearch_reply(client_addr, client_addr_len, ssdp_uuid.data());
            osal_msleep(20);
            ssdp_send_msearch_reply(client_addr, client_addr_len, "urn:schemas-upnp-org:device:MediaRenderer:1",
                                    "urn:schemas-upnp-org:device:MediaRenderer:1");
            osal_msleep(20);
            ssdp_send_msearch_reply(client_addr, client_addr_len, "urn:schemas-upnp-org:service:AVTransport:1",
                                    "urn:schemas-upnp-org:service:AVTransport:1");
            osal_msleep(20);
            ssdp_send_msearch_reply(client_addr, client_addr_len, "urn:schemas-upnp-org:service:RenderingControl:1",
                                    "urn:schemas-upnp-org:service:RenderingControl:1");
            osal_msleep(20);
            ssdp_send_msearch_reply(client_addr, client_addr_len, "urn:schemas-upnp-org:service:ConnectionManager:1",
                                    "urn:schemas-upnp-org:service:ConnectionManager:1");
            return;
        }

        if (is_root) {
            ssdp_send_msearch_reply(client_addr, client_addr_len, "UPnP:rootdevice", "UPnP:rootdevice");
        } else if (is_uuid) {
            ssdp_send_msearch_reply(client_addr, client_addr_len, ssdp_uuid.data());
        } else if (is_renderer) {
            ssdp_send_msearch_reply(client_addr, client_addr_len, st_value.data(), st_value.data());
        } else if (is_av_transport || is_rendering_control || is_connection_manager || is_qplay) {
            ssdp_send_msearch_reply(client_addr, client_addr_len, st_value.data(), st_value.data());
        }
    } else if (is_notify) {
        // 这是别人发出的广播（如路由器的 alive 宣告），直接忽略不打印
    } else {
        osal_printk("ssdp收到未知消息原文:\n%s\n", buffer.data());
    }
}

void dlan::http_process()
{
    int32_t sock = http_sock;
    sockaddr_in client_addr = {0};
    socklen_t client_addr_len = sizeof(client_addr);

    int32_t client_sock = accept(sock, (sockaddr *)&client_addr, &client_addr_len);
    if (client_sock < 0) {
        osal_printk("http接受连接失败\n");
        return;
    }

    // SED : 串口输出
    uint32_t peer_ip = lwip_ntohl(client_addr.sin_addr.s_addr);
    osal_printk("http收到连接: %u.%u.%u.%u:%u\n", (peer_ip >> 24) & 0xFF, (peer_ip >> 16) & 0xFF, (peer_ip >> 8) & 0xFF,
                peer_ip & 0xFF, lwip_ntohs(client_addr.sin_port));

    static std::array<char, 2048> buffer; // HTTP接收缓冲区
    errcode_t ret = lwip_recv(client_sock, buffer.data(), buffer.size() - 1, 0);
    if (ret <= 0) {
        osal_printk("http数据接收失败\n");
        lwip_close(client_sock);
        return;
    }
    buffer[ret] = '\0';

    // AI
    // SED : 串口输出，打印HTTP请求首行（直到\r\n），方便调试验证手机APP的请求格式是否正确。
    char *line_end = strstr(buffer.data(), "\r\n");
    if (line_end != nullptr) {
        *line_end = '\0';
    }
    osal_printk("http请求首行: %s\n", buffer.data());
    if (line_end != nullptr) {
        *line_end = '\r';
    }
    // AI结束

    // 【新增】处理 SUBSCRIBE 事件订阅请求
    if (strstr(buffer.data(), "SUBSCRIBE") != nullptr) {
        osal_printk("http收到 SUBSCRIBE 请求\n");
        // 回复 200 OK + SID + TIMEOUT
        std::array<char, 256> subscribe_response;
        snprintf(subscribe_response.data(), subscribe_response.size(),
                 "HTTP/1.1 200 OK\r\n"
                 "SID: %s\r\n"
                 "TIMEOUT: Second-1800\r\n"
                 "CONTENT-LENGTH: 0\r\n"
                 "Connection: close\r\n\r\n",
                 ssdp_uuid.data());

        lwip_send(client_sock, subscribe_response.data(), strlen(subscribe_response.data()), 0);
        osal_printk("已回复 SUBSCRIBE: SID=%s\n", ssdp_uuid.data());
        lwip_close(client_sock);
        return;
    }

    // 【新增】处理 UNSUBSCRIBE 取消订阅请求
    if (strstr(buffer.data(), "UNSUBSCRIBE") != nullptr) {
        osal_printk("http收到 UNSUBSCRIBE 请求\n");

        static const char *unsubscribe_response =
            "HTTP/1.1 200 OK\r\n"
            "CONTENT-LENGTH: 0\r\n"
            "Connection: close\r\n\r\n";

        lwip_send(client_sock, unsubscribe_response, strlen(unsubscribe_response), 0);
        osal_printk("已回复 UNSUBSCRIBE\n");
        lwip_close(client_sock);
        return;
    }

    // 请求设备描述的XML文件
    if (strstr(buffer.data(), "GET /description.xml") || strstr(buffer.data(), "HEAD /description.xml") ||
        strstr(buffer.data(), "GET / HTTP/1.1") || strstr(buffer.data(), "GET / HTTP/1.0")) {
        // SED : 串口输出
        osal_printk("http收到设备描述请求\n");
        std::array<char, 2048> xml_response;
        std::array<char, 256> header;
        snprintf(xml_response.data(), xml_response.size(),
                 "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                 "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">\r\n"
                 "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
                 "  <device>\r\n"
                 "    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>\r\n"
                 "    <friendlyName>%s</friendlyName>\r\n"
                 "    <manufacturer>%s</manufacturer>\r\n"
                 "    <manufacturerURL>http://www.hisilicon.com/</manufacturerURL>\r\n"
                 "    <modelDescription>%s</modelDescription>\r\n"
                 "    <modelName>WS63-DMR</modelName>\r\n"
                 "    <modelNumber>%.1f</modelNumber>\r\n"
                 "    <UDN>%s</UDN>\r\n"
                 "    <dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>\r\n"
                 "    <serviceList>\r\n"
                 "      <service>\r\n"
                 "        <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>\r\n"
                 "        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>\r\n"
                 "        <SCPDURL>/AVTransport.xml</SCPDURL>\r\n"
                 "        <controlURL>/AVTransport/control</controlURL>\r\n"
                 "        <eventSubURL>/AVTransport/event</eventSubURL>\r\n"
                 "      </service>\r\n"
                 "      <service>\r\n"
                 "        <serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>\r\n"
                 "        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>\r\n"
                 "        <SCPDURL>/RenderingControl.xml</SCPDURL>\r\n"
                 "        <controlURL>/RenderingControl/control</controlURL>\r\n"
                 "        <eventSubURL>/RenderingControl/event</eventSubURL>\r\n"
                 "      </service>\r\n"
                 "      <service>\r\n"
                 "        <serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>\r\n"
                 "        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>\r\n"
                 "        <SCPDURL>/ConnectionManager.xml</SCPDURL>\r\n"
                 "        <controlURL>/ConnectionManager/control</controlURL>\r\n"
                 "        <eventSubURL>/ConnectionManager/event</eventSubURL>\r\n"
                 "      </service>\r\n"
                 "    </serviceList>\r\n"
                 "  </device>\r\n"
                 "</root>\r\n",
                 http_xml_name.data(), http_xml_manufacturer.data(), http_xml_model_description.data(),
                 http_xml_version, ssdp_uuid.data());
        int response_length = strlen(xml_response.data());
        snprintf(header.data(), header.size(),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n"
                 "Server: OS/1.0 UPnP/1.1 product/1.0\r\n\r\n",
                 response_length);
        lwip_send(client_sock, header.data(), strlen(header.data()), 0);
        lwip_send(client_sock, (const uint8_t *)xml_response.data(), strlen(xml_response.data()), 0);
        lwip_close(client_sock);
        return;
    }
    // 回复GET的追加部分
    else if (strstr(buffer.data(), "GET /AVTransport.xml") != nullptr ||
             strstr(buffer.data(), "HEAD /AVTransport.xml") != nullptr) {
        // SED : 串口输出
        osal_printk("http收到AVTransport.xml请求\n");
        static constexpr const char *avt_xml =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
            "  <actionList>\r\n"
            "    <action><name>SetAVTransportURI</name></action>\r\n"
            "    <action><name>Play</name></action>\r\n"
            "    <action><name>Pause</name></action>\r\n"
            "    <action><name>Stop</name></action>\r\n"
            "    <action><name>GetTransportInfo</name></action>\r\n"
            "    <action><name>GetPositionInfo</name></action>\r\n"
            "  </actionList>\r\n"
            "</scpd>";

        std::array<char, 256> header;
        int response_length = strlen(avt_xml);
        snprintf(header.data(), header.size(),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n"
                 "Server: OS/1.0 UPnP/1.1 product/1.0\r\n\r\n",
                 response_length);
        lwip_send(client_sock, header.data(), strlen(header.data()), 0);
        lwip_send(client_sock, (const uint8_t *)avt_xml, response_length, 0);
        lwip_close(client_sock);
        return;
    }
    // 处理rendering xml
    else if (strstr(buffer.data(), "GET /RenderingControl.xml") != nullptr ||
             strstr(buffer.data(), "HEAD /RenderingControl.xml") != nullptr) {
        // SED : 串口输出
        osal_printk("http收到RenderingControl.xml请求\n");
        static constexpr const char *rc_xml =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
            "  <actionList>\r\n"
            "    <action><name>SetVolume</name></action>\r\n"
            "    <action><name>GetVolume</name></action>\r\n"
            "    <action><name>SetMute</name></action>\r\n"
            "    <action><name>GetMute</name></action>\r\n"
            "  </actionList>\r\n"
            "</scpd>";

        std::array<char, 256> header;
        int response_length = strlen(rc_xml);
        snprintf(header.data(), header.size(),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n"
                 "Server: OS/1.0 UPnP/1.1 product/1.0\r\n\r\n",
                 response_length);
        lwip_send(client_sock, header.data(), strlen(header.data()), 0);
        lwip_send(client_sock, (const uint8_t *)rc_xml, response_length, 0);
        lwip_close(client_sock);
        return;
    }
    // 处理connection manager xml
    else if (strstr(buffer.data(), "GET /ConnectionManager.xml") != nullptr ||
             strstr(buffer.data(), "HEAD /ConnectionManager.xml") != nullptr) {
        osal_printk("http收到 ConnectionManager.xml 请求\n");
        static constexpr const char *cm_xml =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
            "  <actionList>\r\n"
            "    <action><name>GetProtocolInfo</name></action>\r\n"
            "    <action><name>GetCurrentConnectionIDs</name></action>\r\n"
            "  </actionList>\r\n"
            "</scpd>";

        std::array<char, 256> header;
        int response_length = strlen(cm_xml);
        snprintf(header.data(), header.size(),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n"
                 "Server: OS/1.0 UPnP/1.1 product/1.0\r\n\r\n",
                 response_length);
        lwip_send(client_sock, header.data(), strlen(header.data()), 0);
        lwip_send(client_sock, (const uint8_t *)cm_xml, response_length, 0);
        lwip_close(client_sock);
        return;
    }
    // 处理SetAVTransportURI等控制命令
    else if (strstr(buffer.data(), "POST") != nullptr) {
        // 判断是否是AVTransport、RenderingControl或ConnectionManager的控制命令
        const bool is_avtransport = strstr(buffer.data(), "POST /AVTransport/control") != nullptr;
        const bool is_renderingcontrol = strstr(buffer.data(), "POST /RenderingControl/control") != nullptr;
        const bool is_connectionmanager = strstr(buffer.data(), "POST /ConnectionManager/control") != nullptr;

        if (!(is_avtransport || is_renderingcontrol || is_connectionmanager)) {
            static const char *unknown_command_response = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
            lwip_send(client_sock, unknown_command_response, strlen(unknown_command_response), 0);
            lwip_close(client_sock);
            return;
        }
        // SED : 串口输出
        osal_printk("http收到控制命令: %s\n",
                    is_avtransport ? "AVTransport" : (is_renderingcontrol ? "RenderingControl" : "ConnectionManager"));
        // 按照SOAPACTION做动作分发
        const char *soap_action_start = strstr(buffer.data(), "SOAPACTION:");
        if (soap_action_start != nullptr) {
            // ========== AVTransport 服务的 SOAP 动作处理 ==========
            if (is_avtransport) {
                if (strstr(soap_action_start, "#SetAVTransportURI") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到SetAVTransportURI命令\n");
                    std::array<char, 512> media_url = {0}; // 从SOAP请求中提取出媒体URL，方便后续实现真正的播放功能
                    extract_xml_tag_value(buffer.data(), "CurrentURI", media_url.data(), media_url.size());
                    static const char *http_answer_av =
                        "HTTP/1.1 200 OK\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:SetAVTransportURIResponse "
                        "xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    lwip_send(client_sock, http_answer_av, strlen(http_answer_av) - 1, 0);

                    // 处理媒体URL，真正实现DLNA播放功能的核心就在这里了，当前先打印出来验证手机APP的请求格式是否正确。
                    osal_printk("SetAVTransportURI的媒体URL: %s\n", media_url.data());

                    // 处理音频
                    if (data_process != nullptr) {
                        data_process(media_url.data(), strlen(media_url.data()));
                    }
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_start, "#Play") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到play相关命令\n");
                    // 回复一个固定的成功响应
                    static const char *play_control_response =
                        "HTTP/1.1 200 OK\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:PlayResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    lwip_send(client_sock, play_control_response, strlen(play_control_response) - 1, 0);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_start, "#Pause") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到pause相关命令\n");
                    static const char *pause_control_response =
                        "HTTP/1.1 200 OK\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:PauseResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    lwip_send(client_sock, pause_control_response, strlen(pause_control_response) - 1, 0);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_start, "#Stop") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到stop相关命令\n");
                    static const char *stop_control_response =
                        "HTTP/1.1 200 OK\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:StopResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    lwip_send(client_sock, stop_control_response, strlen(stop_control_response) - 1, 0);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_start, "#GetTransportInfo") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 GetTransportInfo 命令\n");
                    // 获取传输状态：PLAYING, PAUSED_PLAYBACK, STOPPED, NO_MEDIA_PRESENT
                    static const char *transport_info_response =
                        "HTTP/1.1 200 OK\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetTransportInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
                        "<CurrentTransportState>PLAYING</CurrentTransportState>"
                        "<CurrentTransportStatus>OK</CurrentTransportStatus>"
                        "<CurrentSpeed>1</CurrentSpeed>"
                        "</u:GetTransportInfoResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    lwip_send(client_sock, transport_info_response, strlen(transport_info_response) - 1, 0);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_start, "#GetPositionInfo") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 GetPositionInfo 命令\n");
                    // 获取播放进度信息
                    static const char *position_info_response =
                        "HTTP/1.1 200 OK\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetPositionInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
                        "<Track>1</Track>"
                        "<TrackDuration>00:05:00</TrackDuration>"
                        "<TrackMetaData></TrackMetaData>"
                        "<TrackURI></TrackURI>"
                        "<RelTime>00:00:30</RelTime>"
                        "<AbsTime>00:00:30</AbsTime>"
                        "<RelCount>2147483647</RelCount>"
                        "<AbsCount>2147483647</AbsCount>"
                        "</u:GetPositionInfoResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    lwip_send(client_sock, position_info_response, strlen(position_info_response) - 1, 0);
                    lwip_close(client_sock);
                    return;
                } else {
                    // AVTransport 中未匹配的动作，返回 501
                    osal_printk("http收到AVTransport未知动作\n");
                    static const char *k501 =
                        "HTTP/1.1 501 Not Implemented\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n";
                    lwip_send(client_sock, (const uint8_t *)k501, strlen(k501) - 1, 0);
                    lwip_close(client_sock);
                    return;
                }
            }
            // ========== RenderingControl 服务的 SOAP 动作处理 ==========
            else if (is_renderingcontrol) {
                if (strstr(soap_action_start, "#SetVolume") != nullptr) {
                    //  SED : 串口输出
                    osal_printk("http收到 SetVolume 命令\n");
                    static const char *set_volume_response =
                        "HTTP/1.1 200 OK\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:SetVolumeResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    lwip_send(client_sock, set_volume_response, strlen(set_volume_response) - 1, 0);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_start, "#GetVolume") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 GetVolume 命令\n");
                    static const char *get_volume_response =
                        "HTTP/1.1 200 OK\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetVolumeResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
                        "<CurrentVolume>50</CurrentVolume>"
                        "</u:GetVolumeResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    lwip_send(client_sock, get_volume_response, strlen(get_volume_response) - 1, 0);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_start, "#SetMute") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 SetMute 命令\n");
                    static const char *set_mute_response =
                        "HTTP/1.1 200 OK\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:SetMuteResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    lwip_send(client_sock, set_mute_response, strlen(set_mute_response) - 1, 0);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_start, "#GetMute") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 GetMute 命令\n");
                    static const char *get_mute_response =
                        "HTTP/1.1 200 OK\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetMuteResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
                        "<CurrentMute>0</CurrentMute>"
                        "</u:GetMuteResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    lwip_send(client_sock, get_mute_response, strlen(get_mute_response) - 1, 0);
                    lwip_close(client_sock);
                    return;
                } else {
                    // SED : 串口输出
                    osal_printk("http收到RenderingControl未知动作\n");
                    static const char *k501 =
                        "HTTP/1.1 501 Not Implemented\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n";
                    lwip_send(client_sock, (const uint8_t *)k501, sizeof(k501) - 1, 0);
                    lwip_close(client_sock);
                    return;
                }
            }
            // ========== ConnectionManager 服务的 SOAP 动作处理 ==========
            else if (is_connectionmanager) {
                if (strstr(soap_action_start, "#GetProtocolInfo") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 GetProtocolInfo 命令\n");
                    static const char *protocol_info_response =
                        "HTTP/1.1 200 OK\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetProtocolInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
                        "<Source>http-get:*:audio/mpeg:*,http-get:*:audio/mp4:*</Source>"
                        "<Sink></Sink>"
                        "</u:GetProtocolInfoResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    lwip_send(client_sock, protocol_info_response, strlen(protocol_info_response) - 1, 0);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_start, "#GetCurrentConnectionIDs") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 GetCurrentConnectionIDs 命令\n");
                    static const char *connection_ids_response =
                        "HTTP/1.1 200 OK\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetCurrentConnectionIDsResponse "
                        "xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
                        "<ConnectionIDs></ConnectionIDs>"
                        "</u:GetCurrentConnectionIDsResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    lwip_send(client_sock, connection_ids_response, strlen(connection_ids_response) - 1, 0);
                    lwip_close(client_sock);
                    return;
                } else {
                    // SED : 串口输出
                    osal_printk("http收到ConnectionManager未知动作\n");
                    static const char *k501 =
                        "HTTP/1.1 501 Not Implemented\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n";
                    lwip_send(client_sock, (const uint8_t *)k501, sizeof(k501) - 1, 0);
                    lwip_close(client_sock);
                    return;
                }
            } else {
                // SOAPACTION 头不存在，返回 501
                osal_printk("http收到POST但无SOAPACTION头\n");
                static const char *k501 =
                    "HTTP/1.1 501 Not Implemented\r\n"
                    "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                    "CONNECTION: close\r\n\r\n";
                lwip_send(client_sock, (const uint8_t *)k501, sizeof(k501) - 1, 0);
                lwip_close(client_sock);
                return;
            }
        } else {
            // 非 GET 描述、非 POST 控制，且不在已支持协议范围内：返回 400。
            static const char k400[] = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
            lwip_send(client_sock, (const uint8_t *)k400, sizeof(k400) - 1, 0);
            lwip_close(client_sock);
        }
    }
}

void dlan::ssdp_ip_get()
{
    netif *netif_p = netif_default;
    if (netif_p == nullptr || !netif_is_up(netif_p)) {
        // STA场景优先尝试wlan0，避免netif_default尚未切换完成。
        netif_p = netif_find("wlan0");
    }
    if (netif_p == nullptr || !netif_is_up(netif_p)) {
        snprintf(local_ip.data(), sizeof(local_ip), "0.0.0.0");
        osal_printk("获取默认网络接口失败\n");
        return;
    }

    uint32_t ip_host_order = lwip_ntohl(netif_p->ip_addr.u_addr.ip4.addr);
    snprintf(local_ip.data(), sizeof(local_ip), "%u.%u.%u.%u", (ip_host_order >> 24) & 0xFF,
             (ip_host_order >> 16) & 0xFF, (ip_host_order >> 8) & 0xFF, ip_host_order & 0xFF);
    osal_printk("dlan本地IP=%s\n", local_ip.data());
}

void dlan::dlan_stop()
{
    // 结束DLAN相关的套接字和资源
    if (ssdp_sock >= 0) {
        lwip_close(ssdp_sock);
        ssdp_sock = -1;
    }
    if (http_sock >= 0) {
        lwip_close(http_sock);
        http_sock = -1;
    }
}

void dlan::set_data_clear_fuction(data_clear_t callback)
{
    data_clear = callback;
}

void dlan::set_data_process_fuction(data_process_t callback)
{
    data_process = callback;
}
