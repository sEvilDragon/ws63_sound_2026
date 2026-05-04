#include "control_router.hpp"
#include "playback_bridge/playback_bridge.hpp"
#include "event/event.hpp"

namespace sed_ws63 {

// 添加一些辅助函数
namespace {
const char *find_http_body(const char *request)
{
    if (request == nullptr) {
        return nullptr;
    }
    const char *body = wifi_tool::strstr_s(request, "\r\n\r\n");
    return (body == nullptr) ? nullptr : (body + 4);
}

bool looks_like_mp3_candidate(const char *uri, const char *metadata)
{
    // 第一阶段先沿用当前项目的能力边界，只接收明显的 MP3 资源。
    if (metadata != nullptr && metadata[0] != '\0') {
        if (wifi_tool::is_strstr_ignore_case(metadata, "audio/mpeg") ||
            wifi_tool::is_strstr_ignore_case(metadata, "audio/mp3") ||
            wifi_tool::is_strstr_ignore_case(metadata, "audio/x-mpeg") ||
            wifi_tool::is_strstr_ignore_case(metadata, "mp3")) {
            return true;
        }
        return false;
    }

    if (uri == nullptr || uri[0] == '\0') {
        return false;
    }
    if (wifi_tool::is_strstr_ignore_case(uri, ".mp3") || wifi_tool::is_strstr_ignore_case(uri, "format=mp3") ||
        wifi_tool::is_strstr_ignore_case(uri, "mime=audio/mpeg")) {
        return true;
    }

    return true;
}
} // namespace

errcode_t control_router::handle_get(const char *request,
                                     const char *local_ip,
                                     uint16_t http_port,
                                     const char *udn,
                                     dlna_renderer_state *state,
                                     dlna_control_response *out)
{
    unused(state);

    // 根路径和 description.xml 统一落到设备描述文档，便于手机端直接探活。
    if (strstr(request, "GET /description.xml") != nullptr || strstr(request, "HEAD /description.xml") != nullptr ||
        strstr(request, "GET / HTTP/1.1") != nullptr || strstr(request, "GET / HTTP/1.0") != nullptr ||
        strstr(request, "HEAD / HTTP/1.1") != nullptr || strstr(request, "HEAD / HTTP/1.0") != nullptr) {
        xml::create_description_xml(out->body, sizeof(out->body), local_ip, http_port, udn);
        return ERRCODE_SUCC;
    }

    if (strstr(request, "GET /AVTransport.xml") != nullptr || strstr(request, "HEAD /AVTransport.xml") != nullptr) {
        xml::create_avtransport_service_xml(out->body, sizeof(out->body));
        return ERRCODE_SUCC;
    }

    if (strstr(request, "GET /RenderingControl.xml") != nullptr ||
        strstr(request, "HEAD /RenderingControl.xml") != nullptr) {
        xml::create_renderingcontrol_service_xml(out->body, sizeof(out->body));
        return ERRCODE_SUCC;
    }

    if (strstr(request, "GET /ConnectionManager.xml") != nullptr ||
        strstr(request, "HEAD /ConnectionManager.xml") != nullptr) {
        xml::create_connectionmanager_service_xml(out->body, sizeof(out->body));
        return ERRCODE_SUCC;
    }

    out->http_status = 404;
    (void)snprintf(out->status_text, sizeof(out->status_text), "Not Found");
    (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
    (void)snprintf(out->body, sizeof(out->body), "not found");
    return 0x01; // 无效的请求
}

errcode_t control_router::handle_unsubscribe(const char *request,
                                             subscription_manager *subs,
                                             dlna_control_response *out)
{
    dlna_service_kind kind = dlna_service_kind::unknown;
    if (strstr(request, "UNSUBSCRIBE /AVTransport/event") != nullptr) {
        kind = dlna_service_kind::avtransport;
    } else if (strstr(request, "UNSUBSCRIBE /RenderingControl/event") != nullptr) {
        kind = dlna_service_kind::renderingcontrol;
    } else if (strstr(request, "UNSUBSCRIBE /ConnectionManager/event") != nullptr) {
        kind = dlna_service_kind::connectionmanager;
    }

    if (kind == dlna_service_kind::unknown) {
        out->http_status = 400;
        (void)snprintf(out->status_text, sizeof(out->status_text), "Bad Request");
        (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
        (void)snprintf(out->body, sizeof(out->body), "invalid unsubscribe");
        return 0x01; // 无效的请求
    }

    subs->clear(kind);
    out->http_status = 200;
    (void)snprintf(out->status_text, sizeof(out->status_text), "OK");
    (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
    out->body[0] = '\0';
    return ERRCODE_SUCC;
}

errcode_t control_router::handle_subscribe(const char *request, subscription_manager *subs, dlna_control_response *out)
{
    char sid[128] = {0};
    char callback[256] = {0};
    char timeout[64] = {0};
    uint32_t timeout_seconds = 1800;

    const wifi_tool::span_text sid_span = wifi_tool::find_http_header_value(request, "SID");
    const wifi_tool::span_text callback_span = wifi_tool::find_http_header_value(request, "CALLBACK");
    const wifi_tool::span_text timeout_span = wifi_tool::find_http_header_value(request, "TIMEOUT");

    const bool has_sid =
        sid_span.ptr != nullptr && wifi_tool::copy_str(sid, sizeof(sid), sid_span.ptr, sid_span.len) == ERRCODE_SUCC;
    const bool has_callback =
        callback_span.ptr != nullptr &&
        wifi_tool::copy_str(callback, sizeof(callback), callback_span.ptr, callback_span.len) == ERRCODE_SUCC;
    const bool has_timeout =
        timeout_span.ptr != nullptr &&
        wifi_tool::copy_str(timeout, sizeof(timeout), timeout_span.ptr, timeout_span.len) == ERRCODE_SUCC;

    if (has_callback) {
        wifi_tool::trim_and_strip(callback, '<', '>');
    }
    if (has_timeout) {
        if (wifi_tool::is_strstr_ignore_case(timeout, "Second-")) {
            const char *p = wifi_tool::strstr_ignore_case(timeout, "Second-");
            if (p != nullptr) {
                timeout_seconds = static_cast<uint32_t>(atoi(p + 7));
            }
        }
    }

    dlna_service_kind kind = dlna_service_kind::unknown;
    if (strstr(request, "SUBSCRIBE /AVTransport/event") != nullptr) {
        kind = dlna_service_kind::avtransport;
    } else if (strstr(request, "SUBSCRIBE /RenderingControl/event") != nullptr) {
        kind = dlna_service_kind::renderingcontrol;
    } else if (strstr(request, "SUBSCRIBE /ConnectionManager/event") != nullptr) {
        kind = dlna_service_kind::connectionmanager;
    }

    if (kind == dlna_service_kind::unknown || (!has_sid && !has_callback)) {
        out->http_status = 400;
        (void)snprintf(out->status_text, sizeof(out->status_text), "Bad Request");
        (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
        (void)snprintf(out->body, sizeof(out->body), "invalid subscribe");
        return 0x01; // 无效的请求
    }

    // SUBSCRIBE 有两种形态：首次订阅带 CALLBACK，续租只带 SID。
    const bool is_renew = has_sid && !has_callback;
    if (is_renew) {
        dlna_subscribe_slot *slot = subs->get(kind);
        // 续租只能作用于当前已存在的活动订阅，避免无效 SID 覆盖正常状态。
        if (slot == nullptr || !slot->is_valid || !wifi_tool::strcmp_ignore_case(slot->sid, sid)) {
            out->http_status = 412;
            (void)snprintf(out->status_text, sizeof(out->status_text), "Precondition Failed");
            (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
            (void)snprintf(out->body, sizeof(out->body), "invalid sid");
            return 0x01; // 无效的请求
        }
        slot->timeout_sec = (timeout_seconds == 0) ? slot->timeout_sec : timeout_seconds;
    } else {
        // 首次订阅时创建或覆盖槽位，并把“初始事件待发送”标志带给 renderer。
        const char *sid_to_use = has_sid ? sid : "uuid:ws63-dlna-default-sub";
        subs->upsert(kind, sid_to_use, callback, timeout_seconds);
        if (kind == dlna_service_kind::avtransport) {
            out->send_initial_avtransport_notify = true;
        } else if (kind == dlna_service_kind::renderingcontrol) {
            out->send_initial_rendering_notify = true;
        }
    }

    out->http_status = 200;
    (void)snprintf(out->status_text, sizeof(out->status_text), "OK");
    (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
    out->body[0] = '\0';
    return ERRCODE_SUCC;
}

errcode_t control_router::handle_post(const char *request,
                                      dlna_renderer_state *state,
                                      subscription_manager *subs,
                                      playback_bridge *bridge,
                                      dlna_control_response *out)
{
    unused(subs);

    char soap_action[128] = {0};
    wifi_tool::span_text soap_action_span = wifi_tool::find_http_header_value(request, "SOAPACTION");
    if (soap_action_span.ptr == nullptr || wifi_tool::copy_str(soap_action, sizeof(soap_action), soap_action_span.ptr,
                                                               soap_action_span.len) != ERRCODE_SUCC) {
        out->http_status = 400;
        out->http_status = 500;
        snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
        xml::create_soap_fault_xml(out->body, sizeof(out->body), 401, "Invalid Action");
        return 0x01; // 无效的请求
    }

    wifi_tool::trim_and_strip(soap_action, '"', '"');
    // SED_LOG: 串口调试打印，用于确认SOAP动作已进入控制路由，调试完成后应删除。
    osal_printk("dlna soap action: %s\n", soap_action);
    // 解析URI字段
    const char *body = find_http_body(request);
    if (body == nullptr) {
        xml::create_soap_fault_xml(out->body, sizeof(out->body), 402, "Invalid Args");
        out->http_status = 500;
        snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
        return 0x01; // 无效的请求
    }

    if (wifi_tool::is_strstr_ignore_case(soap_action, "SetAVTransportURI")) {
        char uri[512] = {0};
        char metadata[512] = {0};
        // SetURI 只负责保存和校验资源，不在这里抢先做网络探测。
        if (xml::find_xml_tag_value(body, "CurrentURI", uri, sizeof(uri)) != ERRCODE_SUCC || uri[0] == '\0') {
            xml::create_soap_fault_xml(out->body, sizeof(out->body), 402, "CurrentURI Missing");
            out->http_status = 500;
            snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
            return 0x01; // 无效的请求
        }
        xml::find_xml_tag_value(body, "CurrentURIMetaData", metadata, sizeof(metadata));
        // 控制点经常会把 URI 和 metadata 做 XML 实体转义，这里统一恢复。
        xml::xml_unescape_basic(uri);
        xml::xml_unescape_basic(metadata);

        if (!looks_like_mp3_candidate(uri, metadata)) {
            xml::create_soap_fault_xml(out->body, sizeof(out->body), 714, "Illegal MIME-Type");
            out->http_status = 500;
            snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
            return 0x01; // 无效的请求
        }

        if (bridge->set_uri(uri, metadata) != ERRCODE_SUCC) {
            xml::create_soap_fault_xml(out->body, sizeof(out->body), 701, "Transition not available");
            out->http_status = 500;
            snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
            return 0x01; // 无效的请求
        }

        // bridge 成功后，再把 renderer 自己的镜像状态更新到最新。
        wifi_tool::copy_str(state->current_uri, sizeof(state->current_uri), uri);
        wifi_tool::copy_str(state->current_metadata, sizeof(state->current_metadata), metadata);
        state->state = dlna_transport_state::stopped;
        xml::create_soap_response_xml(out->body, sizeof(out->body), "urn:schemas-upnp-org:service:AVTransport:1",
                                      "SetAVTransportURI", "");
        return ERRCODE_SUCC;
    }

    if (wifi_tool::is_strstr_ignore_case(soap_action, "Play")) {
        if (bridge->play() != ERRCODE_SUCC) {
            (void)xml::create_soap_fault_xml(out->body, sizeof(out->body), 701, "Transition not available");
            out->http_status = 500;
            (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
            return 0x01; // 无效的请求
        }

        state->state = dlna_transport_state::playing;
        (void)xml::create_soap_response_xml(out->body, sizeof(out->body), "urn:schemas-upnp-org:service:AVTransport:1",
                                            "Play", "");
        return ERRCODE_SUCC;
    }

    if (wifi_tool::is_strstr_ignore_case(soap_action, "Pause")) {
        if (bridge->pause() != ERRCODE_SUCC) {
            (void)xml::create_soap_fault_xml(out->body, sizeof(out->body), 701, "Transition not available");
            out->http_status = 500;
            (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
            return 0x01; // 无效的请求
        }

        state->state = dlna_transport_state::paused_playback;
        (void)xml::create_soap_response_xml(out->body, sizeof(out->body), "urn:schemas-upnp-org:service:AVTransport:1",
                                            "Pause", "");
        return ERRCODE_SUCC;
    }

    if (wifi_tool::is_strstr_ignore_case(soap_action, "Stop")) {
        if (bridge->stop() != ERRCODE_SUCC) {
            (void)xml::create_soap_fault_xml(out->body, sizeof(out->body), 701, "Transition not available");
            out->http_status = 500;
            (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
            return 0x01; // 无效的请求
        }

        state->state = dlna_transport_state::stopped;
        (void)xml::create_soap_response_xml(out->body, sizeof(out->body), "urn:schemas-upnp-org:service:AVTransport:1",
                                            "Stop", "");
        return ERRCODE_SUCC;
    }

    if (wifi_tool::is_strstr_ignore_case(soap_action, "GetTransportInfo")) {
        char inner[256] = {0};
        (void)snprintf(inner, sizeof(inner),
                       "<CurrentTransportState>%s</CurrentTransportState>"
                       "<CurrentTransportStatus>OK</CurrentTransportStatus>"
                       "<CurrentSpeed>1</CurrentSpeed>",
                       state->transport_state_text());
        (void)xml::create_soap_response_xml(out->body, sizeof(out->body), "urn:schemas-upnp-org:service:AVTransport:1",
                                            "GetTransportInfo", inner);
        return ERRCODE_SUCC;
    }

    (void)xml::create_soap_fault_xml(out->body, sizeof(out->body), 401, "Invalid Action");
    out->http_status = 500;
    (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
    return 0x01; // 无效的请求
}

errcode_t control_router::handle_request(const char *request,
                                         const char *local_ip,
                                         uint16_t http_port,
                                         const char *udn,
                                         dlna_renderer_state *state,
                                         subscription_manager *subs,
                                         playback_bridge *bridge,
                                         dlna_control_response *out)
{
    if (request == nullptr || state == nullptr || subs == nullptr || bridge == nullptr || out == nullptr) {
        return 0x01; // 无效的请求
    }

    // 每次请求都先拿一份默认响应，再按分支覆盖。
    *out = dlna_control_response{};

    // 这里仅做方法级分发，具体业务逻辑下沉到各 handle_xxx_。
    if (strncmp(request, "GET ", 4) == 0 || strncmp(request, "HEAD ", 5) == 0) {
        return handle_get(request, local_ip, http_port, udn, state, out);
    }
    if (strncmp(request, "SUBSCRIBE ", 10) == 0) {
        return handle_subscribe(request, subs, out);
    }
    if (strncmp(request, "UNSUBSCRIBE ", 12) == 0) {
        return handle_unsubscribe(request, subs, out);
    }
    if (strncmp(request, "POST ", 5) == 0) {
        return handle_post(request, state, subs, bridge, out);
    }

    out->http_status = 400;
    (void)snprintf(out->status_text, sizeof(out->status_text), "Bad Request");
    (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
    (void)snprintf(out->body, sizeof(out->body), "unsupported request");
    return 0x01; // 无效的请求
}

} // namespace sed_ws63