#include "renderer.hpp"

namespace sed_ws63 {
namespace {
static char g_http_request[2048] = {0};
static char g_http_packet[3072] = {0};
static char g_notify_body[1024] = {0};
static dlna_control_response g_http_response;

bool format_ipv4_text(const netif *netif_p, char *out_ip, size_t out_size)
{
    if (netif_p == nullptr || out_ip == nullptr || out_size == 0) {
        return false;
    }

    if (ip4addr_ntoa_r(&(netif_p->ip_addr.u_addr.ip4), out_ip, static_cast<int>(out_size)) == nullptr) {
        return false;
    }

    return strcmp(out_ip, "0.0.0.0") != 0;
}

void log_peer_packet(const char *tag, const sockaddr_in &peer_addr, int payload_len, const char *payload)
{
    if (tag == nullptr || payload == nullptr) {
        return;
    }

    uint32_t peer_ip = lwip_ntohl(peer_addr.sin_addr.s_addr);
    const char *line_end = strstr(payload, "\r\n");
    int line_len = (line_end != nullptr) ? static_cast<int>(line_end - payload) : payload_len;
    if (line_len < 0) {
        line_len = 0;
    }
    if (line_len > 120) {
        line_len = 120;
    }

    // SED_LOG: 串口调试打印，用于确认是否收到了HTTP请求，调试完成后应删除。
    osal_printk("%s: from=%u.%u.%u.%u:%u len=%d first=%.*s\n", tag, (peer_ip >> 24) & 0xFF, (peer_ip >> 16) & 0xFF,
                (peer_ip >> 8) & 0xFF, peer_ip & 0xFF, lwip_ntohs(peer_addr.sin_port), payload_len, line_len, payload);
}

int build_http_response_packet(char *out,
                               size_t out_size,
                               const dlna_control_response &resp,
                               const char *sid_header = nullptr)
{
    const int body_len = static_cast<int>(strlen(resp.body));

    // 只有 SUBSCRIBE 这类响应才需要把 SID 回写给控制点。
    if (sid_header != nullptr && sid_header[0] != '\0') {
        return snprintf(out, out_size,
                        "HTTP/1.1 %d %s\r\n"
                        "CONTENT-TYPE: %s\r\n"
                        "SID: %s\r\n"
                        "TIMEOUT: Second-1800\r\n"
                        "CONTENT-LENGTH: %d\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "%s",
                        resp.http_status, resp.status_text, resp.content_type, sid_header, body_len, resp.body);
    }

    return snprintf(out, out_size,
                    "HTTP/1.1 %d %s\r\n"
                    "CONTENT-TYPE: %s\r\n"
                    "CONTENT-LENGTH: %d\r\n"
                    "CONNECTION: close\r\n\r\n"
                    "%s",
                    resp.http_status, resp.status_text, resp.content_type, body_len, resp.body);
}

} // namespace

renderer::renderer(playback_bridge &bridge) : bridge_(bridge) {}

errcode_t renderer::wait_valid_ip_(char *out_ip, size_t out_size)
{
    if (out_ip == nullptr || out_size == 0) {
        return ERRCODE_FAIL;
    }

    // 保持与旧版dlan一致：优先使用默认接口，仅在默认接口尚未就绪时退回到wlan0。
    while (true) {
        netif *netif_p = netif_default;
        if (netif_p == nullptr || !netif_is_up(netif_p)) {
            netif_p = netif_find("wlan0");
        }

        if (netif_p != nullptr && netif_is_up(netif_p)) {
            if (format_ipv4_text(netif_p, out_ip, out_size)) {
                return ERRCODE_SUCC;
            }
        }
        osal_msleep(500);
    }
}

errcode_t renderer::run()
{
    if (wait_valid_ip_(local_ip_, sizeof(local_ip_)) != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }

    // 先把发现面和控制面监听都拉起来，再进入统一的事件循环。
    if (ssdp_.open_socket() != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }
    if (http_listener_.start_listen(http_port_) != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }

    // SED_LOG: 串口调试打印，用于确认renderer监听已启动，调试完成后应删除。
    osal_printk("dlna renderer ready: ip=%s http_port=%u ssdp_port=%u\n", local_ip_, http_port_, 1900);

    while (true) {
        // renderer 自己只做多路复用和调度，不在这里展开协议细节。
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(ssdp_.get_sfd(), &read_fds);
        FD_SET(http_listener_.get_sfd(), &read_fds);

        const int max_fd = (ssdp_.get_sfd() > http_listener_.get_sfd()) ? ssdp_.get_sfd() : http_listener_.get_sfd();
        if (lwip_select(max_fd + 1, &read_fds, nullptr, nullptr, nullptr) < 0) {
            continue;
        }

        if (FD_ISSET(ssdp_.get_sfd(), &read_fds)) {
            ssdp_.process_once(local_ip_, http_port_, udn_);
        }

        if (FD_ISSET(http_listener_.get_sfd(), &read_fds)) {
            sockaddr_in peer = {};
            int32_t client_fd = http_listener_.accept_one(&peer);
            if (client_fd >= 0) {
                handle_http_client_(client_fd, peer);
            }
        }
    }

    return ERRCODE_SUCC;
}

errcode_t renderer::handle_http_client_(int32_t client_fd, const sockaddr_in &peer_addr)
{
    memset(g_http_request, 0, sizeof(g_http_request));
    int ret = lwip_recv(client_fd, g_http_request, sizeof(g_http_request) - 1, 0);
    if (ret <= 0) {
        // SED_LOG: 串口调试打印，用于确认HTTP接收失败分支是否被命中，调试完成后应删除。
        osal_printk("dlna http recv failed: ret=%d\n", ret);
        lwip_close(client_fd);
        return ERRCODE_FAIL;
    }
    g_http_request[ret] = '\0';
    log_peer_packet("dlna http request", peer_addr, ret, g_http_request);

    // HTTP 解析与 SOAP 分发全部交给 router，renderer 只保留调度职责。
    g_http_response = dlna_control_response{};
    router_.handle_request(g_http_request, local_ip_, http_port_, udn_, &state_, &subscriptions_, &bridge_,
                           &g_http_response);

    const dlna_subscribe_slot *avt = subscriptions_.get(dlna_service_kind::avtransport);
    const dlna_subscribe_slot *rcs = subscriptions_.get(dlna_service_kind::renderingcontrol);
    const dlna_subscribe_slot *cm = subscriptions_.get(dlna_service_kind::connectionmanager);
    const char *sid_header = nullptr;
    // 只有订阅响应需要回写 SID，普通 GET/POST 不需要额外头。
    if (strncmp(g_http_request, "SUBSCRIBE /AVTransport/event", 28) == 0 && avt != nullptr) {
        sid_header = avt->sid;
    } else if (strncmp(g_http_request, "SUBSCRIBE /RenderingControl/event", 33) == 0 && rcs != nullptr) {
        sid_header = rcs->sid;
    } else if (strncmp(g_http_request, "SUBSCRIBE /ConnectionManager/event", 34) == 0 && cm != nullptr) {
        sid_header = cm->sid;
    }

    memset(g_http_packet, 0, sizeof(g_http_packet));
    int packet_len = build_http_response_packet(g_http_packet, sizeof(g_http_packet), g_http_response, sid_header);
    if (packet_len > 0) {
        // SED_LOG: 串口调试打印，用于确认HTTP响应是否成功生成，调试完成后应删除。
        osal_printk("dlna http response: status=%d bytes=%d\n", g_http_response.http_status, packet_len);
        lwip_send(client_fd, g_http_packet, packet_len, 0);
    }
    lwip_close(client_fd);

    return send_initial_notify_if_needed_(g_http_response);
}

errcode_t renderer::send_initial_notify_if_needed_(const dlna_control_response &resp)
{
    if (resp.send_initial_avtransport_notify) {
        dlna_subscribe_slot *slot = subscriptions_.get(dlna_service_kind::avtransport);
        if (slot != nullptr && slot->is_valid) {
            memset(g_notify_body, 0, sizeof(g_notify_body));
            // 初始事件必须在 HTTP 200 回完之后再发，避免控制点还没完成订阅就收到 NOTIFY。
            uint32_t seq = subscriptions_.next_seq(dlna_service_kind::avtransport);
            xml::create_avtransport_event_xml(g_notify_body, sizeof(g_notify_body), state_);
            notifier_.send_event(*slot, seq, g_notify_body);
        }
    }

    if (resp.send_initial_rendering_notify) {
        dlna_subscribe_slot *slot = subscriptions_.get(dlna_service_kind::renderingcontrol);
        if (slot != nullptr && slot->is_valid) {
            memset(g_notify_body, 0, sizeof(g_notify_body));
            uint32_t seq = subscriptions_.next_seq(dlna_service_kind::renderingcontrol);
            xml::create_renderingcontrol_event_xml(g_notify_body, sizeof(g_notify_body), state_);
            notifier_.send_event(*slot, seq, g_notify_body);
        }
    }

    return ERRCODE_SUCC;
}

} // namespace sed_ws63