#include "renderer.hpp"

extern "C" {
#include "soc_osal.h"
}

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

    uint32_t ip_host_order = lwip_ntohl(netif_p->ip_addr.u_addr.ip4.addr);
    snprintf(out_ip, out_size, "%u.%u.%u.%u", (ip_host_order >> 24) & 0xFF, (ip_host_order >> 16) & 0xFF,
             (ip_host_order >> 8) & 0xFF, ip_host_order & 0xFF);
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
                               const char *sid_header = nullptr,
                               bool include_server_header = false,
                               bool append_body = true)
{
    const int body_len = static_cast<int>(strlen(resp.body));
    const char *body = append_body ? resp.body : "";

    if (sid_header != nullptr && sid_header[0] != '\0') {
        return snprintf(out, out_size,
                        "HTTP/1.1 %d %s\r\n"
                        "SID: %s\r\n"
                        "TIMEOUT: Second-1800\r\n"
                        "CONTENT-LENGTH: %d\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "%s",
                        resp.http_status, resp.status_text, sid_header, body_len, body);
    }

    if (include_server_header) {
        return snprintf(out, out_size,
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %d\r\n"
                        "CONNECTION: close\r\n"
                        "Server: OS/1.0 UPnP/1.1 product/1.0\r\n\r\n"
                        "%s",
                        resp.http_status, resp.status_text, resp.content_type, body_len, body);
    }

    return snprintf(out, out_size,
                    "HTTP/1.1 %d %s\r\n"
                    "CONTENT-TYPE: %s\r\n"
                    "CONTENT-LENGTH: %d\r\n"
                    "CONNECTION: close\r\n\r\n"
                    "%s",
                    resp.http_status, resp.status_text, resp.content_type, body_len, body);
}
} // namespace

renderer::renderer(playback_bridge &bridge) : bridge_(bridge) {}

renderer::~renderer() = default;

errcode_t renderer::wait_valid_ip_(char *out_ip, size_t out_size)
{
    if (out_ip == nullptr || out_size == 0) {
        return ERRCODE_FAIL;
    }

    while (true) {
        netif *netif_p = netif_default;
        if (netif_p == nullptr || !netif_is_up(netif_p)) {
            netif_p = netif_find("wlan0");
        }

        if (netif_p != nullptr && netif_is_up(netif_p) && format_ipv4_text(netif_p, out_ip, out_size)) {
            // SED_LOG: 串口调试打印，用于确认激活阶段已经拿到本地IP，调试完成后应删除。
            osal_printk("dlan本地IP=%s\n", out_ip);
            return ERRCODE_SUCC;
        }

        // SED_LOG: 串口调试打印，用于确认DLNA是否卡在等待有效IP阶段，调试完成后应删除。
        osal_printk("dlan等待有效IP中...\n");
        osal_msleep(500);
    }
}

errcode_t renderer::run()
{
    if (wait_valid_ip_(local_ip_, sizeof(local_ip_)) != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }

    if (ssdp_.open_socket() != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }
    if (http_listener_.start_listen(http_port_) != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }

    // SED_LOG: 串口调试打印，用于确认renderer监听已启动，调试完成后应删除。
    osal_printk("dlna renderer ready: ip=%s http_port=%u ssdp_port=%u\n", local_ip_, http_port_, 1900);

    while (true) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(ssdp_.get_sfd(), &read_fds);
        FD_SET(http_listener_.get_sfd(), &read_fds);

        const int max_fd = (ssdp_.get_sfd() > http_listener_.get_sfd()) ? ssdp_.get_sfd() : http_listener_.get_sfd();
        int select_ret = lwip_select(max_fd + 1, &read_fds, nullptr, nullptr, nullptr);
        if (select_ret < 0) {
            // SED_LOG: 串口调试打印，用于确认renderer的select错误分支是否被命中，调试完成后应删除。
            osal_printk("dlna select failed\n");
            continue;
        }

        if (FD_ISSET(ssdp_.get_sfd(), &read_fds)) {
            (void)ssdp_.process_once(local_ip_, http_port_, udn_);
        }

        if (FD_ISSET(http_listener_.get_sfd(), &read_fds)) {
            sockaddr_in peer = {};
            int32_t client_fd = http_listener_.accept_one(&peer);
            if (client_fd < 0) {
                continue;
            }

            handle_http_client_(client_fd, peer);
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

    dlna_renderer_state previous_state = state_;
    g_http_response = dlna_control_response{};
    router_.handle_request(g_http_request, local_ip_, http_port_, udn_, &state_, &subscriptions_, &bridge_,
                           &g_http_response);

    const dlna_subscribe_slot *avt = subscriptions_.get(dlna_service_kind::avtransport);
    const dlna_subscribe_slot *rcs = subscriptions_.get(dlna_service_kind::renderingcontrol);
    const dlna_subscribe_slot *cm = subscriptions_.get(dlna_service_kind::connectionmanager);
    const char *sid_header = nullptr;
    if (strncmp(g_http_request, "SUBSCRIBE /AVTransport/event", 28) == 0 && avt != nullptr) {
        sid_header = avt->sid;
    } else if (strncmp(g_http_request, "SUBSCRIBE /RenderingControl/event", 33) == 0 && rcs != nullptr) {
        sid_header = rcs->sid;
    } else if (strncmp(g_http_request, "SUBSCRIBE /ConnectionManager/event", 34) == 0 && cm != nullptr) {
        sid_header = cm->sid;
    }

    memset(g_http_packet, 0, sizeof(g_http_packet));
    const bool include_server_header =
        (strncmp(g_http_request, "GET ", 4) == 0) || (strncmp(g_http_request, "HEAD ", 5) == 0);
    const int body_len = static_cast<int>(strlen(g_http_response.body));
    if (include_server_header && sid_header == nullptr) {
        int header_len =
            build_http_response_packet(g_http_packet, sizeof(g_http_packet), g_http_response, nullptr, true, false);
        if (header_len > 0) {
            // SED_LOG: 串口调试打印，用于确认description.xml / service xml的响应体是否为空，调试完成后应删除。
            osal_printk("dlna http xml response: status=%d header=%d body=%d total=%d\n", g_http_response.http_status,
                        header_len, body_len, header_len + body_len);
            int header_sent = lwip_send(client_fd, g_http_packet, header_len, 0);
            int body_sent = 0;
            if (body_len > 0) {
                body_sent = lwip_send(client_fd, g_http_response.body, body_len, 0);
            }
            // SED_LOG: 串口调试打印，用于确认XML响应头和响应体是否真实发出，调试完成后应删除。
            osal_printk("dlna http xml sent: header=%d/%d body=%d/%d\n", header_sent, header_len, body_sent, body_len);
        }
    } else {
        int packet_len =
            build_http_response_packet(g_http_packet, sizeof(g_http_packet), g_http_response, sid_header, false, true);
        if (packet_len > 0) {
            // SED_LOG: 串口调试打印，用于确认HTTP响应是否成功生成，调试完成后应删除。
            osal_printk("dlna http response: status=%d bytes=%d\n", g_http_response.http_status, packet_len);
            lwip_send(client_fd, g_http_packet, packet_len, 0);
        }
    }
    lwip_close(client_fd);

    send_initial_notify_if_needed_(g_http_response);
    return send_state_notify_if_needed_(previous_state, g_http_request, g_http_response.http_status);
}

errcode_t renderer::send_initial_notify_if_needed_(const dlna_control_response &resp)
{
    if (resp.send_initial_avtransport_notify) {
        dlna_subscribe_slot *slot = subscriptions_.get(dlna_service_kind::avtransport);
        if (slot != nullptr && slot->is_valid) {
            memset(g_notify_body, 0, sizeof(g_notify_body));
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

errcode_t renderer::send_state_notify_if_needed_(const dlna_renderer_state &previous_state,
                                                 const char *request,
                                                 int http_status)
{
    if (request == nullptr || http_status != 200 || strncmp(request, "POST ", 5) != 0) {
        return ERRCODE_SUCC;
    }

    if (strstr(request, "POST /AVTransport/control") == request) {
        const bool transport_changed = previous_state.state != state_.state ||
                                       strcmp(previous_state.current_uri, state_.current_uri) != 0 ||
                                       strcmp(previous_state.current_metadata, state_.current_metadata) != 0;
        if (!transport_changed) {
            return ERRCODE_SUCC;
        }

        dlna_subscribe_slot *slot = subscriptions_.get(dlna_service_kind::avtransport);
        if (slot != nullptr && slot->is_valid) {
            memset(g_notify_body, 0, sizeof(g_notify_body));
            uint32_t seq = subscriptions_.next_seq(dlna_service_kind::avtransport);
            xml::create_avtransport_event_xml(g_notify_body, sizeof(g_notify_body), state_);
            notifier_.send_event(*slot, seq, g_notify_body);
        }
        return ERRCODE_SUCC;
    }

    if (strstr(request, "POST /RenderingControl/control") == request) {
        const bool rendering_changed = previous_state.volume != state_.volume || previous_state.mute != state_.mute;
        if (!rendering_changed) {
            return ERRCODE_SUCC;
        }

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
