#include "dlan.hpp"
dlan::dlan() {}

// 初始化静态成员变量
std::array<char, 16> dlan::local_ip = {0};
bool dlan::is_ready = false;
int32_t dlan::ssdp_sock = -1;
int32_t dlan::http_sock = -1;
dlan::media_set_uri_handler dlan::media_set_uri_handler_func = nullptr;
dlan::media_play_handler dlan::media_play_handler_func = nullptr;
dlan::media_pause_handler dlan::media_pause_handler_func = nullptr;
dlan::media_stop_handler dlan::media_stop_handler_func = nullptr;

void dlan::register_media_set_uri_handler(media_set_uri_handler handler)
{
    media_set_uri_handler_func = handler;
}

void dlan::register_media_play_handler(media_play_handler handler)
{
    media_play_handler_func = handler;
}

void dlan::register_media_pause_handler(media_pause_handler handler)
{
    media_pause_handler_func = handler;
}

void dlan::register_media_stop_handler(media_stop_handler handler)
{
    media_stop_handler_func = handler;
}

namespace {
std::array<char, 512> g_current_uri = {0};
std::array<char, 256> g_avt_callback = {0};
std::array<char, 128> g_avt_sid = {0};
uint32_t g_avt_seq = 0;
std::array<char, 256> g_rc_callback = {0};
std::array<char, 128> g_rc_sid = {0};
uint32_t g_rc_seq = 0;
uint32_t g_playback_elapsed_base_sec = 0;
unsigned long long g_playback_started_jiffies = 0;
} // namespace

// 定义全局传输状态静态成员
std::array<char, 32> dlan::g_transport_state = {"STOPPED"};

namespace {
bool send_http_notify_request(const char *callback_url, const char *sid, uint32_t seq, const char *body)
{
    if (callback_url == nullptr || sid == nullptr || body == nullptr || callback_url[0] == '\0' || sid[0] == '\0') {
        return false;
    }

    simple_http_url url;
    if (!parse_http_url(callback_url, url)) {
        osal_printk("NOTIFY URL解析失败: %s\n", callback_url);
        return false;
    }

    int32_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        osal_printk("NOTIFY socket创建失败\n");
        return false;
    }

    timeval tv = {2, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(url.port);
    if (!resolve_ipv4_addr(url.host.data(), &addr.sin_addr)) {
        osal_printk("NOTIFY主机解析失败: %s\n", url.host.data());
        lwip_close(sock);
        return false;
    }

    if (connect(sock, (sockaddr *)&addr, sizeof(addr)) != 0) {
        osal_printk("NOTIFY连接失败: %s:%u\n", url.host.data(), url.port);
        lwip_close(sock);
        return false;
    }

    static std::array<char, 1024> request = {0};
    int body_len = static_cast<int>(strlen(body));
    int request_len = snprintf(request.data(), request.size(),
                               "NOTIFY %s HTTP/1.1\r\n"
                               "HOST: %s:%u\r\n"
                               "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                               "NT: upnp:event\r\n"
                               "NTS: upnp:propchange\r\n"
                               "SID: %s\r\n"
                               "SEQ: %u\r\n"
                               "CONTENT-LENGTH: %d\r\n"
                               "Connection: close\r\n\r\n"
                               "%s",
                               url.path.data(), url.host.data(), url.port, sid, seq, body_len, body);
    if (request_len <= 0 || request_len >= static_cast<int>(request.size())) {
        lwip_close(sock);
        return false;
    }

    int ret = lwip_send(sock, request.data(), request_len, 0);
    if (ret <= 0) {
        osal_printk("NOTIFY发送失败\n");
        lwip_close(sock);
        return false;
    }

    static std::array<char, 256> resp = {0};
    int read_ret = lwip_recv(sock, resp.data(), resp.size() - 1, 0);
    if (read_ret > 0) {
        resp[read_ret] = '\0';
        char *line_end = strstr(resp.data(), "\r\n");
        if (line_end != nullptr) {
            *line_end = '\0';
        }
        // SED ： 串口输出
        osal_printk("NOTIFY响应: %s\n", resp.data());
    }

    lwip_close(sock);
    return true;
}

bool send_http_soap_response(int32_t client_sock, const char *body)
{
    if (body == nullptr) {
        return false;
    }

    int body_len = static_cast<int>(strlen(body));
    std::array<char, 256> header = {0};
    int header_len = snprintf(header.data(), header.size(),
                              "HTTP/1.1 200 OK\r\n"
                              "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                              "CONTENT-LENGTH: %d\r\n"
                              "CONNECTION: close\r\n\r\n",
                              body_len);
    if (header_len <= 0 || header_len >= static_cast<int>(header.size())) {
        return false;
    }

    if (lwip_send(client_sock, header.data(), header_len, 0) <= 0) {
        return false;
    }
    if (body_len > 0 && lwip_send(client_sock, (const uint8_t *)body, body_len, 0) <= 0) {
        return false;
    }
    return true;
}

bool notify_avtransport_state(const char *state)
{
    if (state == nullptr || state[0] == '\0' || g_avt_callback[0] == '\0' || g_avt_sid[0] == '\0') {
        return false;
    }

    static std::array<char, 768> body = {0};
    snprintf(body.data(), body.size(),
             "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
             "<e:property>"
             "<LastChange>"
             "&lt;Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\"&gt;"
             "&lt;InstanceID val=\"0\"&gt;"
             "&lt;TransportState val=\"%s\"/&gt;"
             "&lt;/InstanceID&gt;"
             "&lt;/Event&gt;"
             "</LastChange>"
             "</e:property>"
             "</e:propertyset>",
             state);

    bool ok = send_http_notify_request(g_avt_callback.data(), g_avt_sid.data(), g_avt_seq++, body.data());
    osal_printk("AVTransport NOTIFY(%s): %s\n", state, ok ? "成功" : "失败");
    return ok;
}

bool notify_renderingcontrol_state(uint8_t volume, bool mute)
{
    if (g_rc_callback[0] == '\0' || g_rc_sid[0] == '\0') {
        return false;
    }

    static std::array<char, 768> body = {0};
    snprintf(body.data(), body.size(),
             "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
             "<e:property>"
             "<LastChange>"
             "&lt;Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\"&gt;"
             "&lt;InstanceID val=\"0\"&gt;"
             "&lt;Volume channel=\"Master\" val=\"%u\"/&gt;"
             "&lt;Mute channel=\"Master\" val=\"%u\"/&gt;"
             "&lt;/InstanceID&gt;"
             "&lt;/Event&gt;"
             "</LastChange>"
             "</e:property>"
             "</e:propertyset>",
             static_cast<unsigned int>(volume), mute ? 1U : 0U);

    bool ok = send_http_notify_request(g_rc_callback.data(), g_rc_sid.data(), g_rc_seq++, body.data());
    osal_printk("RenderingControl NOTIFY(volume=%u,mute=%u): %s\n", static_cast<unsigned int>(volume), mute ? 1U : 0U,
                ok ? "成功" : "失败");
    return ok;
}

void xml_escape_basic(const char *src, char *dst, size_t dst_size)
{
    if (dst == nullptr || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == nullptr) {
        return;
    }

    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_size; ++si) {
        const char *rep = nullptr;
        switch (src[si]) {
            case '&':
                rep = "&amp;";
                break;
            case '<':
                rep = "&lt;";
                break;
            case '>':
                rep = "&gt;";
                break;
            case '"':
                rep = "&quot;";
                break;
            case '\'':
                rep = "&apos;";
                break;
            default:
                break;
        }

        if (rep != nullptr) {
            size_t rep_len = strlen(rep);
            if (di + rep_len >= dst_size) {
                break;
            }
            memcpy(dst + di, rep, rep_len);
            di += rep_len;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

bool is_transport_playing_state(const char *state)
{
    return ascii_iequals(state, "PLAYING") || ascii_iequals(state, "TRANSITIONING");
}

void format_hms(uint32_t total_seconds, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return;
    }
    uint32_t hours = total_seconds / 3600U;
    uint32_t mins = (total_seconds % 3600U) / 60U;
    uint32_t secs = total_seconds % 60U;
    snprintf(out, out_size, "%02u:%02u:%02u", hours, mins, secs);
}

uint32_t get_playback_elapsed_seconds()
{
    uint32_t elapsed = g_playback_elapsed_base_sec;
    if (is_transport_playing_state(dlan::g_transport_state.data()) && g_playback_started_jiffies != 0) {
        unsigned long long now_jiffies = osal_get_jiffies();
        unsigned long long delta_jiffies = (now_jiffies >= g_playback_started_jiffies)
                                              ? (now_jiffies - g_playback_started_jiffies)
                                              : 0ULL;
        unsigned int delta_ms = osal_jiffies_to_msecs(static_cast<unsigned int>(delta_jiffies));
        elapsed += delta_ms / 1000U;
    }
    return elapsed;
}

void update_transport_state(const char *state, bool notify)
{
    if (state == nullptr || state[0] == '\0') {
        return;
    }

    const bool was_playing = is_transport_playing_state(dlan::g_transport_state.data());
    const bool now_playing = is_transport_playing_state(state);

    if (!was_playing && now_playing) {
        g_playback_started_jiffies = osal_get_jiffies();
    } else if (was_playing && !now_playing && g_playback_started_jiffies != 0) {
        unsigned long long now_jiffies = osal_get_jiffies();
        unsigned long long delta_jiffies =
            (now_jiffies >= g_playback_started_jiffies) ? (now_jiffies - g_playback_started_jiffies) : 0ULL;
        unsigned int delta_ms = osal_jiffies_to_msecs(static_cast<unsigned int>(delta_jiffies));
        g_playback_elapsed_base_sec += delta_ms / 1000U;
        g_playback_started_jiffies = 0;
    }

    if (ascii_iequals(state, "STOPPED")) {
        g_playback_elapsed_base_sec = 0;
        g_playback_started_jiffies = 0;
    }

    copy_string_safe(dlan::g_transport_state.data(), dlan::g_transport_state.size(), state);
    if (notify) {
        notify_avtransport_state(state);
    }
}

bool stream_probe_once(const char *uri)
{
    if (uri == nullptr || uri[0] == '\0') {
        return false;
    }

    simple_http_url url;
    if (!parse_http_url(uri, url)) {
        osal_printk("拉流URL解析失败: %s\n", uri);
        return false;
    }

    int32_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        osal_printk("拉流socket创建失败\n");
        return false;
    }

    timeval tv = {3, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(url.port);
    if (!resolve_ipv4_addr(url.host.data(), &addr.sin_addr)) {
        osal_printk("拉流主机解析失败: %s\n", url.host.data());
        lwip_close(sock);
        return false;
    }

    if (connect(sock, (sockaddr *)&addr, sizeof(addr)) != 0) {
        osal_printk("拉流连接失败: %s:%u\n", url.host.data(), url.port);
        lwip_close(sock);
        return false;
    }

    static std::array<char, 768> request = {0};
    int request_len = snprintf(request.data(), request.size(),
                               "GET %s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Connection: close\r\n"
                               "Icy-MetaData: 1\r\n\r\n",
                               url.path.data(), url.host.data());
    if (request_len <= 0 || request_len >= static_cast<int>(request.size())) {
        lwip_close(sock);
        return false;
    }

    if (lwip_send(sock, request.data(), request_len, 0) <= 0) {
        osal_printk("拉流GET发送失败\n");
        lwip_close(sock);
        return false;
    }

    static std::array<char, 512> recv_buf = {0};
    int recv_len = lwip_recv(sock, recv_buf.data(), recv_buf.size(), 0);
    lwip_close(sock);

    if (recv_len <= 0) {
        osal_printk("拉流首包接收失败\n");
        return false;
    }

    // SED : 串口输出
    osal_printk("拉流首包成功，字节数=%d\n", recv_len);
    return true;
}

void init_dlan_runtime_state_once()
{
    static bool inited = false;
    static constexpr const char *k_default_avt_sid = "uuid:20260321-1612-2007-0423-a1b2c3d4e5f6-avt";
    static constexpr const char *k_default_rc_sid = "uuid:20260321-1612-2007-0423-a1b2c3d4e5f6-rc";
    if (inited) {
        return;
    }

    copy_string_safe(g_avt_sid.data(), g_avt_sid.size(), k_default_avt_sid);
    copy_string_safe(g_rc_sid.data(), g_rc_sid.size(), k_default_rc_sid);
    copy_string_safe(dlan::g_transport_state.data(), dlan::g_transport_state.size(), "STOPPED");
    g_playback_elapsed_base_sec = 0;
    g_playback_started_jiffies = 0;
    inited = true;
}
} // namespace

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

    // SED : 串口输出
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
    ssdp_addr.sin_port = lwip_htons(ssdp_port);

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
    http_addr.sin_port = lwip_htons(http_port);
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
        bool has_st = extract_http_header_value(buffer.data(), "ST", st_value.data(), st_value.size());
        bool has_man = extract_http_header_value(buffer.data(), "MAN", man_value.data(), man_value.size());
        bool has_host = extract_http_header_value(buffer.data(), "HOST", host_value.data(), host_value.size());
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
    init_dlan_runtime_state_once();

    int32_t sock = http_sock;
    sockaddr_in client_addr = {0};
    socklen_t client_addr_len = sizeof(client_addr);

    int32_t client_sock = lwip_accept(sock, (sockaddr *)&client_addr, &client_addr_len);
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
        std::array<char, 256> sid_value = {0};
        std::array<char, 256> callback_value = {0};
        const bool has_sid = extract_http_header_value(buffer.data(), "SID", sid_value.data(), sid_value.size());
        const bool has_callback =
            extract_http_header_value(buffer.data(), "CALLBACK", callback_value.data(), callback_value.size());
        if (has_sid) {
            trim_ascii_whitespace(sid_value.data());
        }
        if (has_callback) {
            trim_ascii_whitespace(callback_value.data());
            strip_angle_brackets(callback_value.data());
        }

        const bool is_avtransport_event = strstr(buffer.data(), "SUBSCRIBE /AVTransport/event") != nullptr;
        const bool is_rendering_event = strstr(buffer.data(), "SUBSCRIBE /RenderingControl/event") != nullptr;
        if (is_avtransport_event) {
            if (has_sid && sid_value[0] != '\0') {
                copy_string_safe(g_avt_sid.data(), g_avt_sid.size(), sid_value.data());
            }
            if (has_callback && callback_value[0] != '\0') {
                copy_string_safe(g_avt_callback.data(), g_avt_callback.size(), callback_value.data());
            }
            g_avt_seq = 0;
            osal_printk("AVTransport订阅: SID=%s CALLBACK=%s\n", g_avt_sid.data(), g_avt_callback.data());
        } else if (is_rendering_event) {
            if (has_sid && sid_value[0] != '\0') {
                copy_string_safe(g_rc_sid.data(), g_rc_sid.size(), sid_value.data());
            }
            if (has_callback && callback_value[0] != '\0') {
                copy_string_safe(g_rc_callback.data(), g_rc_callback.size(), callback_value.data());
            }
            g_rc_seq = 0;
            osal_printk("RenderingControl订阅: SID=%s CALLBACK=%s\n", g_rc_sid.data(), g_rc_callback.data());
        }

        const char *sid_to_reply = ssdp_uuid.data();
        if (is_avtransport_event && g_avt_sid[0] != '\0') {
            sid_to_reply = g_avt_sid.data();
        } else if (is_rendering_event && g_rc_sid[0] != '\0') {
            sid_to_reply = g_rc_sid.data();
        }

        // 回复 200 OK + SID + TIMEOUT
        std::array<char, 256> subscribe_response;
        snprintf(subscribe_response.data(), subscribe_response.size(),
                 "HTTP/1.1 200 OK\r\n"
                 "SID: %s\r\n"
                 "TIMEOUT: Second-1800\r\n"
                 "CONTENT-LENGTH: 0\r\n"
                 "Connection: close\r\n\r\n",
                 sid_to_reply);

        lwip_send(client_sock, subscribe_response.data(), strlen(subscribe_response.data()), 0);
        osal_printk("已回复 SUBSCRIBE: SID=%s\n", sid_to_reply);
        lwip_close(client_sock);
        if (is_avtransport_event) {
            notify_avtransport_state(dlan::g_transport_state.data());
        } else if (is_rendering_event) {
            notify_renderingcontrol_state(50, false);
        }
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
        static std::array<char, 2048> xml_response;
        static std::array<char, 256> header;
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
            "    <action><name>GetCurrentConnectionInfo</name></action>\r\n"
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
        std::array<char, 256> soap_action_value = {0};
        const bool has_soap_action =
            extract_http_header_value(buffer.data(), "SOAPACTION", soap_action_value.data(), soap_action_value.size());
        if (has_soap_action) {
            trim_ascii_whitespace(soap_action_value.data());
            size_t action_len = strlen(soap_action_value.data());
            if (action_len >= 2 && soap_action_value[0] == '"' && soap_action_value[action_len - 1] == '"') {
                memmove(soap_action_value.data(), soap_action_value.data() + 1, action_len - 2);
                soap_action_value[action_len - 2] = '\0';
            }

            // ========== AVTransport 服务的 SOAP 动作处理 ==========
            if (is_avtransport) {
                if (strstr(soap_action_value.data(), "#SetAVTransportURI") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到SetAVTransportURI命令\n");
                    static std::array<char, 512> media_url = {0}; // 从SOAP请求中提取出媒体URL，方便后续实现真正的播放功能
                    extract_xml_tag_value(buffer.data(), "CurrentURI", media_url.data(), media_url.size());
                    html_entity_decode_amp(media_url.data());
                    copy_string_safe(g_current_uri.data(), g_current_uri.size(), media_url.data());
                    if (media_set_uri_handler_func != nullptr) {
                        media_set_uri_handler_func(g_current_uri.data());
                    }
                    static const char *set_uri_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:SetAVTransportURIResponse "
                        "xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, set_uri_response_body);

                    // 处理媒体URL，真正实现DLNA播放功能的核心就在这里了，当前先打印出来验证手机APP的请求格式是否正确。
                    osal_printk("SetAVTransportURI的媒体URL: %s\n", media_url.data());

                    // 先上报STOPPED，让控制端确认URI设置已生效。
                    update_transport_state("STOPPED", true);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_value.data(), "#Play") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到play相关命令\n");
                    // 回复一个固定的成功响应
                    static const char *play_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:PlayResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, play_response_body);

                    // 进入播放前上报TRANSITIONING。
                    // 不在控制通道里做同步拉流探测，避免额外连接影响真实播放链路稳定性。
                    update_transport_state("TRANSITIONING", true);
                    bool play_ok = false;
                    if (media_play_handler_func != nullptr) {
                        play_ok = media_play_handler_func(g_current_uri.data());
                    } else {
                        play_ok = (g_current_uri[0] != '\0');
                    }
                    if (play_ok) {
                        update_transport_state("PLAYING", true);
                    } else {
                        update_transport_state("STOPPED", true);
                    }

                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_value.data(), "#Pause") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到pause相关命令\n");
                    static const char *pause_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:PauseResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, pause_response_body);
                    if (media_pause_handler_func != nullptr) {
                        media_pause_handler_func();
                    }
                    update_transport_state("PAUSED_PLAYBACK", true);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_value.data(), "#Stop") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到stop相关命令\n");
                    static const char *stop_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:StopResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, stop_response_body);
                    if (media_stop_handler_func != nullptr) {
                        media_stop_handler_func();
                    }
                    update_transport_state("STOPPED", true);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_value.data(), "#GetTransportInfo") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 GetTransportInfo 命令\n");
                    // 获取传输状态：PLAYING, PAUSED_PLAYBACK, STOPPED, NO_MEDIA_PRESENT
                    static std::array<char, 640> transport_info_body = {0};
                    snprintf(transport_info_body.data(), transport_info_body.size(),
                             "<?xml version=\"1.0\"?>"
                             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                             "<s:Body>"
                             "<u:GetTransportInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
                             "<CurrentTransportState>%s</CurrentTransportState>"
                             "<CurrentTransportStatus>OK</CurrentTransportStatus>"
                             "<CurrentSpeed>1</CurrentSpeed>"
                             "</u:GetTransportInfoResponse>"
                             "</s:Body>"
                             "</s:Envelope>",
                             dlan::g_transport_state.data());
                    send_http_soap_response(client_sock, transport_info_body.data());
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_value.data(), "#GetPositionInfo") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 GetPositionInfo 命令\n");
                    static std::array<char, 1024> escaped_uri = {0};
                    xml_escape_basic(g_current_uri.data(), escaped_uri.data(), escaped_uri.size());

                    const bool has_uri = g_current_uri[0] != '\0';
                    uint32_t elapsed_sec = has_uri ? get_playback_elapsed_seconds() : 0U;
                    static std::array<char, 16> elapsed_hms = {0};
                    format_hms(elapsed_sec, elapsed_hms.data(), elapsed_hms.size());

                    static std::array<char, 1280> position_info_body = {0};
                    snprintf(position_info_body.data(), position_info_body.size(),
                             "<?xml version=\"1.0\"?>"
                             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                             "<s:Body>"
                             "<u:GetPositionInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
                             "<Track>%u</Track>"
                             "<TrackDuration>%s</TrackDuration>"
                             "<TrackMetaData>NOT_IMPLEMENTED</TrackMetaData>"
                             "<TrackURI>%s</TrackURI>"
                             "<RelTime>%s</RelTime>"
                             "<AbsTime>%s</AbsTime>"
                             "<RelCount>2147483647</RelCount>"
                             "<AbsCount>2147483647</AbsCount>"
                             "</u:GetPositionInfoResponse>"
                             "</s:Body>"
                             "</s:Envelope>",
                             has_uri ? 1U : 0U, "00:00:00", has_uri ? escaped_uri.data() : "",
                             elapsed_hms.data(), elapsed_hms.data());
                    send_http_soap_response(client_sock, position_info_body.data());
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
                if (strstr(soap_action_value.data(), "#SetVolume") != nullptr) {
                    //  SED : 串口输出
                    osal_printk("http收到 SetVolume 命令\n");
                    static const char *set_volume_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:SetVolumeResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, set_volume_response_body);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_value.data(), "#GetVolume") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 GetVolume 命令\n");
                    static const char *get_volume_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetVolumeResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
                        "<CurrentVolume>50</CurrentVolume>"
                        "</u:GetVolumeResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, get_volume_response_body);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_value.data(), "#SetMute") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 SetMute 命令\n");
                    static const char *set_mute_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:SetMuteResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, set_mute_response_body);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_value.data(), "#GetMute") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 GetMute 命令\n");
                    static const char *get_mute_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetMuteResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
                        "<CurrentMute>0</CurrentMute>"
                        "</u:GetMuteResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, get_mute_response_body);
                    lwip_close(client_sock);
                    return;
                } else {
                    // SED : 串口输出
                    osal_printk("http收到RenderingControl未知动作\n");
                    static const char *k501 =
                        "HTTP/1.1 501 Not Implemented\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n";
                    lwip_send(client_sock, (const uint8_t *)k501, strlen(k501), 0);
                    lwip_close(client_sock);
                    return;
                }
            }
            // ========== ConnectionManager 服务的 SOAP 动作处理 ==========
            else if (is_connectionmanager) {
                if (strstr(soap_action_value.data(), "#GetProtocolInfo") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 GetProtocolInfo 命令\n");
                    static const char *protocol_info_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetProtocolInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
                        "<Source></Source>"
                        "<Sink>http-get:*:audio/mpeg:*,http-get:*:audio/mp3:*</Sink>"
                        "</u:GetProtocolInfoResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, protocol_info_response_body);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_value.data(), "#GetCurrentConnectionIDs") != nullptr) {
                    // SED : 串口输出
                    osal_printk("http收到 GetCurrentConnectionIDs 命令\n");
                    static const char *connection_ids_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetCurrentConnectionIDsResponse "
                        "xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
                        "<ConnectionIDs></ConnectionIDs>"
                        "</u:GetCurrentConnectionIDsResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, connection_ids_response_body);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_value.data(), "#GetCurrentConnectionInfo") != nullptr) {
                    osal_printk("http收到 GetCurrentConnectionInfo 命令\n");
                    static const char *connection_info_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetCurrentConnectionInfoResponse "
                        "xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
                        "<RcsID>-1</RcsID>"
                        "<AVTransportID>-1</AVTransportID>"
                        "<ProtocolInfo></ProtocolInfo>"
                        "<PeerConnectionManager></PeerConnectionManager>"
                        "<PeerConnectionID>-1</PeerConnectionID>"
                        "<Direction>Input</Direction>"
                        "<Status>OK</Status>"
                        "</u:GetCurrentConnectionInfoResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, connection_info_response_body);
                    lwip_close(client_sock);
                    return;
                } else {
                    // SED : 串口输出
                    osal_printk("http收到ConnectionManager未知动作\n");
                    static const char *k501 =
                        "HTTP/1.1 501 Not Implemented\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n";
                    lwip_send(client_sock, (const uint8_t *)k501, strlen(k501), 0);
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
                lwip_send(client_sock, (const uint8_t *)k501, strlen(k501), 0);
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