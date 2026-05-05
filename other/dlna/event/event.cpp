#include "event.hpp"

namespace sed_ws63 {

// 定义一些辅助性函数
namespace {
dlna_subscribe_slot make_empty_slot()
{
    dlna_subscribe_slot slot = {};
    slot.timeout_sec = 1800; // 默认30分钟
    slot.is_valid = false;
    return slot;
}
} // namespace

subscription_manager::subscription_manager()
{
    avtransport_ = make_empty_slot();
    rendering_control_ = make_empty_slot();
    connection_manager_ = make_empty_slot();
}

dlna_subscribe_slot *subscription_manager::get(dlna_service_kind kind)
{
    switch (kind) {
        case dlna_service_kind::avtransport:
            return &avtransport_;
        case dlna_service_kind::renderingcontrol:
            return &rendering_control_;
        case dlna_service_kind::connectionmanager:
            return &connection_manager_;
        default:
            return nullptr;
    }
}

const dlna_subscribe_slot *subscription_manager::get(dlna_service_kind kind) const
{
    switch (kind) {
        case dlna_service_kind::avtransport:
            return &avtransport_;
        case dlna_service_kind::renderingcontrol:
            return &rendering_control_;
        case dlna_service_kind::connectionmanager:
            return &connection_manager_;
        default:
            return nullptr;
    }
}

void subscription_manager::clear(dlna_service_kind kind)
{
    dlna_subscribe_slot *slot = get(kind);
    if (slot) {
        *slot = make_empty_slot();
    }
}

void subscription_manager::upsert(dlna_service_kind kind, const char *sid, const char *callback, uint32_t timeout_sec)
{
    dlna_subscribe_slot *slot = get(kind);
    if (slot == nullptr) {
        return;
    }

    *slot = make_empty_slot();
    slot->is_valid = true;
    slot->timeout_sec = timeout_sec;
    wifi_tool::copy_str(slot->sid, sizeof(slot->sid), sid);
    wifi_tool::copy_str(slot->callback_url, sizeof(slot->callback_url), callback);
}

uint32_t subscription_manager::next_seq(dlna_service_kind kind)
{
    dlna_subscribe_slot *slot = get(kind);
    if (slot == nullptr) {
        return 0;
    }
    return slot->seq++;
}

errcode_t notify_sender::send_event(const dlna_subscribe_slot &slot, uint32_t seq, const char *body)
{
    if (slot.is_valid == false || slot.sid[0] == '\0' || slot.callback_url[0] == '\0' || body == nullptr) {
        return 0x01; // 无效的订阅槽
    }

    wifi_tool::parse_url url = {};
    // 这里仍然只接收http协议的URL，其他协议不处理
    errcode_t ret = wifi_tool::get_url(slot.callback_url, url);
    if (ret != ERRCODE_SUCC) {
        return 0x02; // 无效的URL
    }
    if (wifi_tool::strcmp_ignore_case(url.scheme.data(), "http") == false) {
        return 0x02; // 无效的URL
    }

    tcp_client client;
    ret = client.connect_host(url.host.data(), url.port, 2000);
    if (ret != ERRCODE_SUCC) {
        return 0x03; // 连接主机失败
    }

    char request[1536] = {0};
    int body_len = static_cast<int>(strlen(body));
    int req_len = snprintf(request, sizeof(request),
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
                           url.path.data(), url.host.data(), url.port, slot.sid, seq, body_len, body);
    if (req_len <= 0 || req_len >= static_cast<int>(sizeof(request))) {
        return 0x04; // 请求内容过长
    }

    ret = client.send_data(request, static_cast<uint32_t>(req_len));
    if (ret < 100) {
        return 0x05; // 发送数据失败
    }

    // 作简短读取，读取掉对端响应
    char response[256] = {0};
    client.recv_data(response, sizeof(response)-1,1000);

    return ERRCODE_SUCC;
}

} // namespace sed_ws63