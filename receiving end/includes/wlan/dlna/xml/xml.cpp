#include "xml.hpp"

namespace sed_ws63 {

errcode_t xml::xml_escape_basic(char *dst, size_t dst_size, const char *src)
{
    if (dst == nullptr || dst_size == 0 || src == nullptr) {
        return 0x01; // 参数错误
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
            for (size_t ri = 0; rep[ri] != '\0'; ++ri) {
                if (di + 1 >= dst_size) {
                    dst[di] = '\0';
                    return 0x02; // 输出缓冲区不足，但已经尽力写入了
                }
                dst[di++] = rep[ri];
            }
            continue;
        }

        if (di + 1 >= dst_size) {
            dst[di] = '\0';
            return 0x02; // 输出缓冲区不足，但已经尽力写入了
        }
        dst[di++] = src[si];
    }

    dst[di] = '\0';
    return ERRCODE_SUCC; // 成功
}

errcode_t xml::xml_unescape_basic(char *text)
{
    if (text == nullptr) {
        return 0x01; // 参数错误
    }

    return wifi_tool::decode_xml_basic(text);
}

errcode_t xml::find_xml_tag_value(const char *text, const char *tag_name, char *value_out, size_t value_out_size)
{
    if (text == nullptr || tag_name == nullptr || value_out == nullptr || value_out_size == 0) {
        return 0x01; // 参数错误
    }

    wifi_tool::span_text value_span;
    value_span = wifi_tool::find_html_tag_value(text, tag_name);
    if (value_span.ptr == nullptr) {
        value_out[0] = '\0';
        return ERRCODE_SUCC; // 没有找到标签，但这不算错误，输出空字符串
    }

    // 确保不会出界
    size_t copy_len = (value_span.len < value_out_size - 1) ? value_span.len : (value_out_size - 1);
    wifi_tool::copy_str(value_out, value_out_size, value_span.ptr, copy_len);
    return ERRCODE_SUCC; // 成功
}

errcode_t xml::create_description_xml(char *out,
                                      size_t out_size,
                                      const char *local_ip,
                                      uint16_t http_port,
                                      const char *udn)
{
    if (out == nullptr || out_size == 0 || local_ip == nullptr || udn == nullptr) {
        return 0x01; // 参数错误
    }

    snprintf(out, out_size,
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
             "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" "
             "xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">\r\n"
             "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
             "  <device>\r\n"
             "    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>\r\n"
             "    <friendlyName>ws63_sound</friendlyName>\r\n"
             "    <manufacturer>sEvil_Dragon</manufacturer>\r\n"
             "    <modelDescription>Audio Device Based on HiSilicon WS63</modelDescription>\r\n"
             "    <modelName>WS63-DMR</modelName>\r\n"
             "    <modelNumber>1.0</modelNumber>\r\n"
             "    <UDN>%s</UDN>\r\n"
             "    <presentationURL>http://%s:%u/</presentationURL>\r\n"
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
             udn, local_ip, http_port);

    return ERRCODE_SUCC;
}

errcode_t xml::create_avtransport_service_xml(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return 0x01; // 参数错误
    }

    snprintf(out, out_size,
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
             "</scpd>");

    return ERRCODE_SUCC;
}

errcode_t xml::create_renderingcontrol_service_xml(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return 0x01; // 参数错误
    }

    snprintf(out, out_size,
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
             "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
             "  <actionList>\r\n"
             "    <action><name>SetVolume</name></action>\r\n"
             "    <action><name>GetVolume</name></action>\r\n"
             "    <action><name>SetMute</name></action>\r\n"
             "    <action><name>GetMute</name></action>\r\n"
             "  </actionList>\r\n"
             "</scpd>");

    return ERRCODE_SUCC;
}

errcode_t xml::create_connectionmanager_service_xml(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return 0x01; // 参数错误
    }

    snprintf(out, out_size,
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
             "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
             "  <actionList>\r\n"
             "    <action><name>GetProtocolInfo</name></action>\r\n"
             "    <action><name>GetCurrentConnectionIDs</name></action>\r\n"
             "    <action><name>GetCurrentConnectionInfo</name></action>\r\n"
             "  </actionList>\r\n"
             "</scpd>");

    return ERRCODE_SUCC;
}

errcode_t xml::create_soap_response_xml(char *out,
                                        size_t out_size,
                                        const char *service_ns,
                                        const char *action_name,
                                        const char *inner_xml)
{
    if (out == nullptr || out_size == 0 || service_ns == nullptr || action_name == nullptr) {
        return 0x01; // 参数错误
    }

    if (inner_xml == nullptr) {
        inner_xml = "";
    }

    snprintf(out, out_size,
             "<?xml version=\"1.0\"?>"
             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
             "<s:Body>"
             "<u:%sResponse xmlns:u=\"%s\">%s</u:%sResponse>"
             "</s:Body>"
             "</s:Envelope>",
             action_name, service_ns, inner_xml, action_name);

    return ERRCODE_SUCC;
}

errcode_t xml::create_soap_fault_xml(char *out, size_t out_size, int upnp_error_code, const char *description)
{
    if (out == nullptr || out_size == 0) {
        return 0x01; // 参数错误
    }

    if (description == nullptr) {
        description = "Invalid Args";
    }

    snprintf(out, out_size,
             "<?xml version=\"1.0\"?>"
             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
             "<s:Body><s:Fault><faultcode>s:Client</faultcode>"
             "<faultstring>UPnPError</faultstring><detail>"
             "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
             "<errorCode>%d</errorCode><errorDescription>%s</errorDescription>"
             "</UPnPError></detail></s:Fault></s:Body></s:Envelope>",
             upnp_error_code, description);

    return ERRCODE_SUCC;
}

errcode_t xml::create_avtransport_event_xml(char *out, size_t out_size, const dlna_renderer_state &state)
{
    if (out == nullptr || out_size == 0) {
        return 0x01; // 参数错误
    }

    snprintf(out, out_size,
             "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
             "<e:property><LastChange>"
             "&lt;Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\"&gt;"
             "&lt;InstanceID val=\"0\"&gt;"
             "&lt;TransportState val=\"%s\"/&gt;"
             "&lt;/InstanceID&gt;"
             "&lt;/Event&gt;"
             "</LastChange></e:property></e:propertyset>",
             state.transport_state_text());

    return ERRCODE_SUCC;
}

errcode_t xml::create_renderingcontrol_event_xml(char *out, size_t out_size, const dlna_renderer_state &state)
{
    if (out == nullptr || out_size == 0) {
        return 0x01; // 参数错误
    }

    return snprintf(out, out_size,
                    "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
                    "<e:property><LastChange>"
                    "&lt;Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\"&gt;"
                    "&lt;InstanceID val=\"0\"&gt;"
                    "&lt;Volume channel=\"Master\" val=\"%u\"/&gt;"
                    "&lt;Mute channel=\"Master\" val=\"%u\"/&gt;"
                    "&lt;/InstanceID&gt;"
                    "&lt;/Event&gt;"
                    "</LastChange></e:property></e:propertyset>",
                    static_cast<unsigned int>(state.volume), state.mute ? 1U : 0U);

    return ERRCODE_SUCC;
}

} // namespace sed_ws63