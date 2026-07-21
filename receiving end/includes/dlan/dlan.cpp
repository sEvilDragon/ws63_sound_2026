#include "dlan.hpp"
#include "minimp3.hpp"
dlan::dlan() {}

// ��ʼ����̬��Ա����
std::array<char, 16> dlan::local_ip = {0};
bool dlan::is_ready = false;
int32_t dlan::ssdp_sock = -1;
int32_t dlan::http_sock = -1;
dlan::media_set_uri_handler dlan::media_set_uri_handler_func = nullptr;
dlan::media_play_handler dlan::media_play_handler_func = nullptr;
dlan::media_pause_handler dlan::media_pause_handler_func = nullptr;
dlan::media_stop_handler dlan::media_stop_handler_func = nullptr;
dlan::media_seek_handler dlan::media_seek_handler_func = nullptr;
volatile bool dlan::s_stop_requested = false;

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

void dlan::register_media_seek_handler(media_seek_handler handler)
{
    media_seek_handler_func = handler;
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

// ����ȫ�ִ���״̬��̬��Ա
std::array<char, 32> dlan::g_transport_state = {"STOPPED"};

namespace {
bool send_http_notify_request(const char *callback_url, const char *sid, uint32_t seq, const char *body)
{
    if (callback_url == nullptr || sid == nullptr || body == nullptr || callback_url[0] == '\0' || sid[0] == '\0') {
        return false;
    }

    simple_http_url url;
    if (!parse_http_url(callback_url, url)) {
        osal_printk("NOTIFY URL����ʧ��: %s\n", callback_url);
        return false;
    }

    int32_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        osal_printk("NOTIFY socket����ʧ��\n");
        return false;
    }

    timeval tv = {2, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(url.port);
    if (!resolve_ipv4_addr(url.host.data(), &addr.sin_addr)) {
        osal_printk("NOTIFY��������ʧ��: %s\n", url.host.data());
        lwip_close(sock);
        return false;
    }

    if (connect(sock, (sockaddr *)&addr, sizeof(addr)) != 0) {
        osal_printk("NOTIFY����ʧ��: %s:%u\n", url.host.data(), url.port);
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
        osal_printk("NOTIFY����ʧ��\n");
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

bool send_http_soap_fault_response(int32_t client_sock, int upnp_error_code, const char *description)
{
    if (description == nullptr) {
        description = "Invalid Args";
    }

    std::array<char, 768> body = {0};
    snprintf(body.data(), body.size(),
             "<?xml version=\"1.0\"?>"
             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
             "<s:Body>"
             "<s:Fault>"
             "<faultcode>s:Client</faultcode>"
             "<faultstring>UPnPError</faultstring>"
             "<detail>"
             "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
             "<errorCode>%d</errorCode>"
             "<errorDescription>%s</errorDescription>"
             "</UPnPError>"
             "</detail>"
             "</s:Fault>"
             "</s:Body>"
             "</s:Envelope>",
             upnp_error_code, description);

    int body_len = static_cast<int>(strlen(body.data()));
    std::array<char, 256> header = {0};
    int header_len = snprintf(header.data(), header.size(),
                              "HTTP/1.1 500 Internal Server Error\r\n"
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
    if (body_len > 0 && lwip_send(client_sock, (const uint8_t *)body.data(), body_len, 0) <= 0) {
        return false;
    }
    return true;
}

bool is_mp3_candidate_from_uri_or_metadata(const char *uri, const char *metadata)
{
    const bool has_metadata = (metadata != nullptr && metadata[0] != '\0');
    if (has_metadata) {
        if (ascii_icontains(metadata, "audio/mpeg") || ascii_icontains(metadata, "audio/mp3") ||
            ascii_icontains(metadata, "audio/x-mpeg") || ascii_icontains(metadata, "mp3")) {
            return true;
        }
        return false;
    }

    if (uri == nullptr || uri[0] == '\0') {
        return false;
    }
    if (ascii_icontains(uri, ".mp3") || ascii_icontains(uri, "format=mp3") || ascii_icontains(uri, "mime=audio/mpeg")) {
        return true;
    }

    // ��Ԫ������URLҲ������չʱ���У�������ɱǩ��ֱ����
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
    osal_printk("AVTransport NOTIFY(%s): %s\n", state, ok ? "�ɹ�" : "ʧ��");
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
                ok ? "�ɹ�" : "ʧ��");
    return ok;
}

void sanitize_http_body_preview(const char *src, char *dst, size_t dst_size)
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
        char ch = src[si];
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            if (di > 0 && dst[di - 1] != ' ') {
                dst[di++] = ' ';
            }
            continue;
        }
        dst[di++] = ch;
    }
    dst[di] = '\0';
}

void log_avtransport_soap_payload(const char *action_name, const char *body_start)
{
    if (action_name == nullptr || body_start == nullptr || body_start[0] == '\0') {
        return;
    }

    std::array<char, 32> instance_id = {0};
    std::array<char, 32> unit = {0};
    std::array<char, 32> target = {0};
    std::array<char, 32> speed = {0};
    std::array<char, 192> preview = {0};

    extract_xml_tag_value(body_start, "InstanceID", instance_id.data(), instance_id.size());
    extract_xml_tag_value(body_start, "Unit", unit.data(), unit.size());
    extract_xml_tag_value(body_start, "Target", target.data(), target.size());
    extract_xml_tag_value(body_start, "Speed", speed.data(), speed.size());
    sanitize_http_body_preview(body_start, preview.data(), preview.size());

    osal_printk("AVTransport SOAP[%s]: InstanceID=%s Unit=%s Target=%s Speed=%s\n", action_name,
                instance_id[0] != '\0' ? instance_id.data() : "-", unit[0] != '\0' ? unit.data() : "-",
                target[0] != '\0' ? target.data() : "-", speed[0] != '\0' ? speed.data() : "-");
    osal_printk("AVTransport SOAPԤ��[%s]: %s\n", action_name, preview.data());
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

bool soap_action_has(const char *soap_action, const char *action_name)
{
    if (soap_action == nullptr || action_name == nullptr) {
        return false;
    }
    return ascii_icontains(soap_action, action_name);
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
        unsigned long long delta_jiffies =
            (now_jiffies >= g_playback_started_jiffies) ? (now_jiffies - g_playback_started_jiffies) : 0ULL;
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
        osal_printk("����URL����ʧ��: %s\n", uri);
        return false;
    }

    int32_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        osal_printk("����socket����ʧ��\n");
        return false;
    }

    timeval tv = {3, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(url.port);
    if (!resolve_ipv4_addr(url.host.data(), &addr.sin_addr)) {
        osal_printk("������������ʧ��: %s\n", url.host.data());
        lwip_close(sock);
        return false;
    }

    if (connect(sock, (sockaddr *)&addr, sizeof(addr)) != 0) {
        osal_printk("��������ʧ��: %s:%u\n", url.host.data(), url.port);
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
        osal_printk("����GET����ʧ��\n");
        lwip_close(sock);
        return false;
    }

    static std::array<char, 512> recv_buf = {0};
    int recv_len = lwip_recv(sock, recv_buf.data(), recv_buf.size(), 0);
    lwip_close(sock);

    if (recv_len <= 0) {
        osal_printk("�����װ�����ʧ��\n");
        return false;
    }

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
        osal_printk("ssdp��Ӧ����ʧ��(ST=%s)\n", st);
        return false;
    }

    return true;
}

void dlan::ssdp_and_http_scan()
{
    while (!is_ready && !s_stop_requested) {
        // �ȴ�����׼����������ȡ����IP��ַ����Ϣ
        osal_msleep(100);
    }
    // WiFi���ӳɹ���������DHCP����ɣ��ȴ��õ���ЧIP������DLNA��
    while (true) {
        ssdp_ip_get();
        if (strcmp(local_ip.data(), "0.0.0.0") != 0) {
            break;
        }
        osal_printk("dlan�ȴ���ЧIP��...\n");
        osal_msleep(500);
    }
    // ����SSDP��UDP���׽���
    ssdp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ssdp_sock < 0) {
        osal_printk("ssdp��������ʧ��\n");
        return;
    }
    if (!ssdp_set()) {
        dlan_stop();
        return;
    }

    // ����HTTP��TCP���׽���
    http_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (http_sock < 0) {
        osal_printk("http��������ʧ��\n");
        dlan_stop();
        return;
    }
    if (!http_set()) {
        dlan_stop();
        return;
    }

    // SED : �������
    osal_printk("dlan�����ɹ���local_ip=%s, ssdp=%d, http=%d\n", local_ip.data(), ssdp_sock, http_sock);

    fd_set read_fds;
    memset(&read_fds, 0, sizeof(read_fds));
    int max_fd = (ssdp_sock > http_sock) ? ssdp_sock : http_sock;

    while (!s_stop_requested) {
        memset(&read_fds, 0, sizeof(read_fds));
        FD_SET(ssdp_sock, &read_fds); // �� UDP ��ؼӽ�ȥ
        FD_SET(http_sock, &read_fds); // �� TCP ��ؼӽ�ȥ

        timeval tv = {1, 0};
        int ret = lwip_select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);

        if (s_stop_requested) break;
        if (ret < 0) {
            osal_printk("select����\n");
            continue;
        }

        if (FD_ISSET(ssdp_sock, &read_fds)) {
            ssdp_process();
        }
        if (FD_ISSET(http_sock, &read_fds)) {
            http_process();
        }
    }

    dlan_stop();
    osal_printk("[DLNA] exited\r\n");
}

bool dlan::ssdp_set()
{
    int32_t sock = ssdp_sock;
    int32_t reuse = 1;

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        osal_printk("ssdp�׽�������SO_REUSEADDRʧ��\n");
    }

    // ����SSDP�׽���
    sockaddr_in ssdp_addr = {0};
    ssdp_addr.sin_family = AF_INET;
    ssdp_addr.sin_addr.s_addr = INADDR_ANY;
    ssdp_addr.sin_port = lwip_htons(ssdp_port);

    errcode_t ret = bind(sock, (sockaddr *)&ssdp_addr, sizeof(ssdp_addr));
    if (ret != 0) {
        osal_printk("ssdp�׽��ְ�ʧ��\n");
        lwip_close(sock);
        ssdp_sock = -1;
        return false;
    }

    // ����ಥ��
    ip_mreq mreq = {0};
    mreq.imr_multiaddr.s_addr = inet_addr(mcast_ip.data());
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        osal_printk("ssdp�׽��ּ���ಥ��ʧ��\n");
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
        osal_printk("http�׽�������SO_REUSEADDRʧ��\n");
    }

    // ����HTTP�׽���
    sockaddr_in http_addr = {0};
    http_addr.sin_family = AF_INET;
    http_addr.sin_addr.s_addr = INADDR_ANY;
    http_addr.sin_port = lwip_htons(http_port);
    errcode_t ret = bind(sock, (sockaddr *)&http_addr, sizeof(http_addr));
    if (ret != 0) {
        osal_printk("http�׽��ְ�ʧ��\n");
        lwip_close(sock);
        http_sock = -1;
        return false;
    }

    // ����listen��������HTTP�׽��֣��������������Ϊ2
    ret = listen(sock, 2);
    if (ret != 0) {
        osal_printk("http�׽��ּ���ʧ��\n");
        lwip_close(sock);
        http_sock = -1;
        return false;
    }

    // SED : �������
    osal_printk("http�������������˿�=%u\n", http_port);

    return true;
}

void dlan::ssdp_process()
{
    int32_t sock = ssdp_sock;
    static std::array<char, 1024> buffer; // SSDP���ջ�����
    sockaddr_in client_addr = {0};        // ��¼SSDP��Ϣ��Դ
    socklen_t client_addr_len = sizeof(client_addr);

    int ret = recvfrom(sock, buffer.data(), buffer.size() - 1, 0, (sockaddr *)&client_addr, &client_addr_len);

    if (ret < 0) {
        osal_printk("ssdp���ݽ���ʧ��\n");
        return;
    }

    buffer[ret] = '\0';

    // ���� M-SEARCH ��������
    bool is_msearch = strstr(buffer.data(), "M-SEARCH") != nullptr;
    // ���� NOTIFY �㲥֪ͨ������ֻ��Ҫ������������������
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
            return;
        }

        if (has_host && strstr(host_value.data(), "239.255.255.250:1900") == nullptr) {
            return;
        }

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
        // ���Ǳ��˷����Ĺ㲥����·������ alive ���棩��ֱ�Ӻ��Բ���ӡ
    } else {
        osal_printk("ssdp�յ�δ֪��Ϣԭ��:\n%s\n", buffer.data());
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
        osal_printk("http��������ʧ��\n");
        return;
    }

    uint32_t peer_ip = lwip_ntohl(client_addr.sin_addr.s_addr);

    static std::array<char, 2048> buffer; // HTTP���ջ�����
    errcode_t ret = lwip_recv(client_sock, buffer.data(), buffer.size() - 1, 0);
    if (ret <= 0) {
        osal_printk("http���ݽ���ʧ��\n");
        lwip_close(client_sock);
        return;
    }
    buffer[ret] = '\0';

    const char *body_start = "";
    char *body_separator = strstr(buffer.data(), "\r\n\r\n");
    if (body_separator != nullptr) {
        body_start = body_separator + 4;
    }

    // ������������ SUBSCRIBE �¼���������
    if (strstr(buffer.data(), "SUBSCRIBE") != nullptr) {
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
            osal_printk("AVTransport����: SID=%s CALLBACK=%s\n", g_avt_sid.data(), g_avt_callback.data());
        } else if (is_rendering_event) {
            if (has_sid && sid_value[0] != '\0') {
                copy_string_safe(g_rc_sid.data(), g_rc_sid.size(), sid_value.data());
            }
            if (has_callback && callback_value[0] != '\0') {
                copy_string_safe(g_rc_callback.data(), g_rc_callback.size(), callback_value.data());
            }
            g_rc_seq = 0;
            osal_printk("RenderingControl����: SID=%s CALLBACK=%s\n", g_rc_sid.data(), g_rc_callback.data());
        }

        const char *sid_to_reply = ssdp_uuid.data();
        if (is_avtransport_event && g_avt_sid[0] != '\0') {
            sid_to_reply = g_avt_sid.data();
        } else if (is_rendering_event && g_rc_sid[0] != '\0') {
            sid_to_reply = g_rc_sid.data();
        }

        // �ظ� 200 OK + SID + TIMEOUT
        std::array<char, 256> subscribe_response;
        snprintf(subscribe_response.data(), subscribe_response.size(),
                 "HTTP/1.1 200 OK\r\n"
                 "SID: %s\r\n"
                 "TIMEOUT: Second-1800\r\n"
                 "CONTENT-LENGTH: 0\r\n"
                 "Connection: close\r\n\r\n",
                 sid_to_reply);

        lwip_send(client_sock, subscribe_response.data(), strlen(subscribe_response.data()), 0);
        lwip_close(client_sock);
        if (is_avtransport_event) {
            notify_avtransport_state(dlan::g_transport_state.data());
        } else if (is_rendering_event) {
            notify_renderingcontrol_state(50, false);
        }
        return;
    }

    // ������������ UNSUBSCRIBE ȡ����������
    if (strstr(buffer.data(), "UNSUBSCRIBE") != nullptr) {
        static const char *unsubscribe_response =
            "HTTP/1.1 200 OK\r\n"
            "CONTENT-LENGTH: 0\r\n"
            "Connection: close\r\n\r\n";

        lwip_send(client_sock, unsubscribe_response, strlen(unsubscribe_response), 0);
        lwip_close(client_sock);
        return;
    }

    // �����豸������XML�ļ�
    if (strstr(buffer.data(), "GET /description.xml") || strstr(buffer.data(), "HEAD /description.xml") ||
        strstr(buffer.data(), "GET / HTTP/1.1") || strstr(buffer.data(), "GET / HTTP/1.0")) {
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
    // �ظ�GET��׷�Ӳ���
    else if (strstr(buffer.data(), "GET /AVTransport.xml") != nullptr ||
             strstr(buffer.data(), "HEAD /AVTransport.xml") != nullptr) {
        static constexpr const char *avt_xml =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
            "  <actionList>\r\n"
            "    <action><name>SetAVTransportURI</name></action>\r\n"
            "    <action><name>Play</name></action>\r\n"
            "    <action><name>Pause</name></action>\r\n"
            "    <action><name>Stop</name></action>\r\n"
            "    <action><name>GetMediaInfo</name></action>\r\n"
            "    <action><name>GetDeviceCapabilities</name></action>\r\n"
            "    <action><name>GetTransportInfo</name></action>\r\n"
            "    <action><name>GetTransportSettings</name></action>\r\n"
            "    <action><name>GetCurrentTransportActions</name></action>\r\n"
            "    <action><name>GetPositionInfo</name></action>\r\n"
            "    <action><name>Seek</name></action>\r\n"
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
    // ����rendering xml
    else if (strstr(buffer.data(), "GET /RenderingControl.xml") != nullptr ||
             strstr(buffer.data(), "HEAD /RenderingControl.xml") != nullptr) {
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
    // ����connection manager xml
    else if (strstr(buffer.data(), "GET /ConnectionManager.xml") != nullptr ||
             strstr(buffer.data(), "HEAD /ConnectionManager.xml") != nullptr) {
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
    // ����SetAVTransportURI�ȿ�������
    else if (strstr(buffer.data(), "POST") != nullptr) {
        // �ж��Ƿ���AVTransport��RenderingControl��ConnectionManager�Ŀ�������
        const bool is_avtransport = strstr(buffer.data(), "POST /AVTransport/control") != nullptr;
        const bool is_renderingcontrol = strstr(buffer.data(), "POST /RenderingControl/control") != nullptr;
        const bool is_connectionmanager = strstr(buffer.data(), "POST /ConnectionManager/control") != nullptr;

        if (!(is_avtransport || is_renderingcontrol || is_connectionmanager)) {
            static const char *unknown_command_response = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
            lwip_send(client_sock, unknown_command_response, strlen(unknown_command_response), 0);
            lwip_close(client_sock);
            return;
        }
        // ����SOAPACTION�������ַ�
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
            // ========== AVTransport ����� SOAP �������� ==========
            if (is_avtransport) {
                if (soap_action_has(soap_action_value.data(), "SetAVTransportURI")) {
                    static std::array<char, 512> media_url = {
                        0}; // ��SOAP��������ȡ��ý��URL���������ʵ�������Ĳ��Ź���
                    static std::array<char, 1024> media_metadata = {0};
                    extract_xml_tag_value(buffer.data(), "CurrentURI", media_url.data(), media_url.size());
                    extract_xml_tag_value(buffer.data(), "CurrentURIMetaData", media_metadata.data(),
                                          media_metadata.size());
                    html_entity_decode_amp(media_url.data());
                    html_entity_decode_amp(media_metadata.data());

                    if (!is_mp3_candidate_from_uri_or_metadata(media_url.data(), media_metadata.data())) {
                        osal_printk("SetAVTransportURI�ܾ�: ��MP3ý�� URI=%s\n", media_url.data());
                        send_http_soap_fault_response(client_sock, 714, "Illegal MIME-Type");
                        lwip_close(client_sock);
                        return;
                    }

                    copy_string_safe(g_current_uri.data(), g_current_uri.size(), media_url.data());
                    bool set_uri_ok = true;
                    if (media_set_uri_handler_func != nullptr) {
                        set_uri_ok = media_set_uri_handler_func(g_current_uri.data());
                    }
                    if (!set_uri_ok) {
                        osal_printk("SetAVTransportURI handler failed for relay URL\n");
                        send_http_soap_fault_response(client_sock, 501, "Relay request failed");
                        update_transport_state("STOPPED", true);
                        lwip_close(client_sock);
                        return;
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

                    // ����ý��URL������ʵ��DLNA���Ź��ܵĺ��ľ��������ˣ���ǰ�ȴ�ӡ������֤�ֻ�APP�������ʽ�Ƿ���ȷ��
                    osal_printk("SetAVTransportURI��ý��URL: %s\n", media_url.data());

                    // ���ϱ�STOPPED���ÿ��ƶ�ȷ��URI��������Ч��
                    update_transport_state("STOPPED", true);
                    lwip_close(client_sock);
                    return;
                } else if (soap_action_has(soap_action_value.data(), "SetNextAVTransportURI")) {
                    static const char *set_next_uri_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:SetNextAVTransportURIResponse "
                        "xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, set_next_uri_response_body);
                    lwip_close(client_sock);
                    return;
                } else if (soap_action_has(soap_action_value.data(), "Play")) {
                    // �ظ�һ���̶��ĳɹ���Ӧ
                    static const char *play_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:PlayResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, play_response_body);

                    // ���벥��ǰ�ϱ�TRANSITIONING��
                    // ���ڿ���ͨ������ͬ������̽�⣬�����������Ӱ����ʵ������·�ȶ��ԡ�
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
                } else if (soap_action_has(soap_action_value.data(), "Pause")) {
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
                } else if (soap_action_has(soap_action_value.data(), "Stop")) {
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
                } else if (soap_action_has(soap_action_value.data(), "Seek")) {
                    // ���� <Target>HH:MM:SS</Target>
                    std::array<char, 32> seek_target = {0};
                    extract_xml_tag_value(body_start, "Target", seek_target.data(), seek_target.size());
                    uint32_t seek_seconds = 0;
                    {
                        unsigned int hh = 0, mm = 0, ss = 0;
                        if (sscanf(seek_target.data(), "%u:%u:%u", &hh, &mm, &ss) == 3) {
                            seek_seconds = hh * 3600U + mm * 60U + ss;
                        }
                    }
                    osal_printk("Seek Ŀ��: %s = %u ��\n", seek_target.data(), (unsigned)seek_seconds);
                    // ���½��ȸ��ٻ�׼
                    g_playback_elapsed_base_sec = seek_seconds;
                    g_playback_started_jiffies = osal_get_jiffies();
                    static const char *seek_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:SeekResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, seek_response_body);
                    if (media_seek_handler_func != nullptr) {
                        media_seek_handler_func(seek_seconds);
                    }
                    lwip_close(client_sock);
                    return;
                } else if (soap_action_has(soap_action_value.data(), "GetMediaInfo")) {
                    static std::array<char, 1024> escaped_uri = {0};
                    xml_escape_basic(g_current_uri.data(), escaped_uri.data(), escaped_uri.size());

                    const bool has_uri = g_current_uri[0] != '\0';
                    uint32_t dur_sec = has_uri ? minimp3::get_duration_seconds() : 0U;
                    static std::array<char, 16> dur_hms = {0};
                    format_hms(dur_sec, dur_hms.data(), dur_hms.size());

                    static std::array<char, 1536> media_info_body = {0};
                    snprintf(media_info_body.data(), media_info_body.size(),
                             "<?xml version=\"1.0\"?>"
                             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                             "<s:Body>"
                             "<u:GetMediaInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
                             "<NrTracks>%u</NrTracks>"
                             "<MediaDuration>%s</MediaDuration>"
                             "<CurrentURI>%s</CurrentURI>"
                             "<CurrentURIMetaData></CurrentURIMetaData>"
                             "<NextURI></NextURI>"
                             "<NextURIMetaData></NextURIMetaData>"
                             "<PlayMedium>NETWORK</PlayMedium>"
                             "<RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
                             "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>"
                             "</u:GetMediaInfoResponse>"
                             "</s:Body>"
                             "</s:Envelope>",
                             has_uri ? 1U : 0U, dur_hms.data(), has_uri ? escaped_uri.data() : "");
                    send_http_soap_response(client_sock, media_info_body.data());
                    lwip_close(client_sock);
                    return;
                } else if (soap_action_has(soap_action_value.data(), "GetDeviceCapabilities")) {
                    static const char *device_capabilities_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetDeviceCapabilitiesResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
                        "<PlayMedia>NETWORK</PlayMedia>"
                        "<RecMedia>NOT_IMPLEMENTED</RecMedia>"
                        "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>"
                        "</u:GetDeviceCapabilitiesResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, device_capabilities_body);
                    lwip_close(client_sock);
                    return;
                } else if (soap_action_has(soap_action_value.data(), "GetTransportInfo")) {
                    // ��ȡ����״̬��PLAYING, PAUSED_PLAYBACK, STOPPED, NO_MEDIA_PRESENT
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
                } else if (soap_action_has(soap_action_value.data(), "GetTransportSettings")) {
                    static const char *transport_settings_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetTransportSettingsResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
                        "<PlayMode>NORMAL</PlayMode>"
                        "<RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>"
                        "</u:GetTransportSettingsResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, transport_settings_body);
                    lwip_close(client_sock);
                    return;
                } else if (soap_action_has(soap_action_value.data(), "GetCurrentTransportActions")) {
                    const bool has_uri = g_current_uri[0] != '\0';
                    const char *actions = "";
                    if (ascii_iequals(dlan::g_transport_state.data(), "PLAYING") ||
                        ascii_iequals(dlan::g_transport_state.data(), "TRANSITIONING")) {
                        actions = "Pause,Seek,Stop";
                    } else if (ascii_iequals(dlan::g_transport_state.data(), "PAUSED_PLAYBACK")) {
                        actions = "Play,Seek,Stop";
                    } else if (has_uri) {
                        actions = "Play,Stop";
                    }

                    static std::array<char, 768> current_actions_body = {0};
                    snprintf(current_actions_body.data(), current_actions_body.size(),
                             "<?xml version=\"1.0\"?>"
                             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                             "<s:Body>"
                             "<u:GetCurrentTransportActionsResponse "
                             "xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
                             "<Actions>%s</Actions>"
                             "</u:GetCurrentTransportActionsResponse>"
                             "</s:Body>"
                             "</s:Envelope>",
                             actions);
                    send_http_soap_response(client_sock, current_actions_body.data());
                    lwip_close(client_sock);
                    return;
                } else if (soap_action_has(soap_action_value.data(), "GetPositionInfo")) {
                    static std::array<char, 1024> escaped_uri = {0};
                    xml_escape_basic(g_current_uri.data(), escaped_uri.data(), escaped_uri.size());

                    const bool has_uri = g_current_uri[0] != '\0';
                    uint32_t elapsed_sec = has_uri ? get_playback_elapsed_seconds() : 0U;
                    uint32_t dur_sec = has_uri ? minimp3::get_duration_seconds() : 0U;
                    static std::array<char, 16> elapsed_hms = {0};
                    static std::array<char, 16> dur_hms = {0};
                    format_hms(elapsed_sec, elapsed_hms.data(), elapsed_hms.size());
                    format_hms(dur_sec, dur_hms.data(), dur_hms.size());

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
                             has_uri ? 1U : 0U, dur_hms.data(), has_uri ? escaped_uri.data() : "", elapsed_hms.data(),
                             elapsed_hms.data());
                    send_http_soap_response(client_sock, position_info_body.data());
                    lwip_close(client_sock);
                    return;
                } else {
                    // AVTransport ��δƥ��Ķ��������� 501
                    osal_printk("http�յ�AVTransportδ֪����: %s\n", soap_action_value.data());
                    static const char *k501 =
                        "HTTP/1.1 501 Not Implemented\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n";
                    lwip_send(client_sock, (const uint8_t *)k501, strlen(k501) - 1, 0);
                    lwip_close(client_sock);
                    return;
                }
            }
            // ========== RenderingControl ����� SOAP �������� ==========
            else if (is_renderingcontrol) {
                if (strstr(soap_action_value.data(), "#SetVolume") != nullptr) {
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
                    // SED : �������
                    osal_printk("http�յ�RenderingControlδ֪����\n");
                    static const char *k501 =
                        "HTTP/1.1 501 Not Implemented\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n";
                    lwip_send(client_sock, (const uint8_t *)k501, strlen(k501), 0);
                    lwip_close(client_sock);
                    return;
                }
            }
            // ========== ConnectionManager ����� SOAP �������� ==========
            else if (is_connectionmanager) {
                if (strstr(soap_action_value.data(), "#GetProtocolInfo") != nullptr) {
                    static const char *protocol_info_response_body =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body>"
                        "<u:GetProtocolInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
                        "<Source></Source>"
                        "<Sink>"
                        "http-get:*:audio/mpeg:*,"
                        "http-get:*:audio/mp3:*,"
                        "https-get:*:audio/mpeg:*,"
                        "https-get:*:audio/mp3:*"
                        "</Sink>"
                        "</u:GetProtocolInfoResponse>"
                        "</s:Body>"
                        "</s:Envelope>";
                    send_http_soap_response(client_sock, protocol_info_response_body);
                    lwip_close(client_sock);
                    return;
                } else if (strstr(soap_action_value.data(), "#GetCurrentConnectionIDs") != nullptr) {
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
                    // SED : �������
                    osal_printk("http�յ�ConnectionManagerδ֪����\n");
                    static const char *k501 =
                        "HTTP/1.1 501 Not Implemented\r\n"
                        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                        "CONNECTION: close\r\n\r\n";
                    lwip_send(client_sock, (const uint8_t *)k501, strlen(k501), 0);
                    lwip_close(client_sock);
                    return;
                }
            } else {
                // SOAPACTION ͷ�����ڣ����� 501
                osal_printk("http�յ�POST����SOAPACTIONͷ\n");
                static const char *k501 =
                    "HTTP/1.1 501 Not Implemented\r\n"
                    "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                    "CONNECTION: close\r\n\r\n";
                lwip_send(client_sock, (const uint8_t *)k501, strlen(k501), 0);
                lwip_close(client_sock);
                return;
            }
        } else {
            // �� GET �������� POST ���ƣ��Ҳ�����֧��Э�鷶Χ�ڣ����� 400��
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
        // STA�������ȳ���wlan0������netif_default��δ�л���ɡ�
        netif_p = netif_find("wlan0");
    }
    if (netif_p == nullptr || !netif_is_up(netif_p)) {
        snprintf(local_ip.data(), sizeof(local_ip), "0.0.0.0");
        osal_printk("��ȡĬ������ӿ�ʧ��\n");
        return;
    }

    uint32_t ip_host_order = lwip_ntohl(netif_p->ip_addr.u_addr.ip4.addr);
    snprintf(local_ip.data(), sizeof(local_ip), "%u.%u.%u.%u", (ip_host_order >> 24) & 0xFF,
             (ip_host_order >> 16) & 0xFF, (ip_host_order >> 8) & 0xFF, ip_host_order & 0xFF);
}

void dlan::dlan_stop()
{
    // ����DLAN��ص��׽��ֺ���Դ
    if (ssdp_sock >= 0) {
        lwip_close(ssdp_sock);
        ssdp_sock = -1;
    }
    if (http_sock >= 0) {
        lwip_close(http_sock);
        http_sock = -1;
    }
}

void dlan::request_stop()
{
    s_stop_requested = true;
}

void dlan::reset_stop()
{
    s_stop_requested = false;
}
