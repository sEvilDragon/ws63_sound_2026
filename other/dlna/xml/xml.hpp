#pragma once

/*
    sed_ws63: xml.hpp
    实现xml基础
*/

extern "C" {
#include "common_def.h"
#include "soc_osal.h"
#include "errcode.h"
}
#include "wifi_tool.hpp"
#include "dlna_types/dlna_types.hpp"

namespace sed_ws63 {

class xml {
public:
private:
public:
    /*
        功能：创建xml描述文件
        参数：
            out：输出缓冲区，xml文本将被写入此处
            out_size：输出缓冲区大小
            local_ip：设备本地IP地址字符串
            http_port：HTTP服务端口号
            udn: 设备UDN字符串
        错误码：
            0x01:参数错误
    */
    static errcode_t create_description_xml(char *out,
                                            size_t out_size,
                                            const char *local_ip,
                                            uint16_t http_port,
                                            const char *udn);
    /*
        功能：
            创建avtransport_service的xml文本
        参数：
            out：输出缓冲区，xml文本将被写入此处
            out_size：输出缓冲区大小
        错误码：
            0x01:参数错误
    */
    static errcode_t create_avtransport_service_xml(char *out, size_t out_size);
    /*
        功能：
            创建renderingcontrol_service的xml文本
        参数：
            out：输出缓冲区，xml文本将被写入此处
            out_size：输出缓冲区大小
        错误码：
            0x01:参数错误
    */
    static errcode_t create_renderingcontrol_service_xml(char *out, size_t out_size);
    /*
        功能：
            创建connectionmanager_service的xml文本
        参数：
            out：输出缓冲区，xml文本将被写入此处
            out_size：输出缓冲区大小
        错误码：
            0x01:参数错误
    */
    static errcode_t create_connectionmanager_service_xml(char *out, size_t out_size);
    /*
        功能：创建soapaction响应xml
        参数：
            out：输出缓冲区，xml文本将被写入此处
            out_size：输出缓冲区大小
            service_ns: 服务命名空间，如"AVTransport"
            action_name: 操作名称，如"GetTransportInfo"
            inner_xml:
       内部xml内容，如"<CurrentTransportState>PLAYING</CurrentTransportState>"，默认是nullptr表示没有内容 错误码：
            0x01:参数错误
    */
    static errcode_t create_soap_response_xml(char *out,
                                              size_t out_size,
                                              const char *service_ns,
                                              const char *action_name,
                                              const char *inner_xml = nullptr);
    /*
        功能：创建soapfault响应xml
        参数：
            out：输出缓冲区，xml文本将被写入此处
            out_size：输出缓冲区大小
            error_code: 错误码，UPnP定义的错误码，如701表示"Transition not available"
            error_description: 错误描述字符串
        错误码：
            0x01:参数错误
    */
    static errcode_t create_soap_fault_xml(char *out,
                                           size_t out_size,
                                           int upnp_error_code,
                                           const char *description = nullptr);
    /*
        功能：创建avtransport_event的xml文本
        参数：
            out：输出缓冲区，xml文本将被写入此处
            out_size：输出缓冲区大小
            state：状态值
        错误码：
            0x01:参数错误
    */
    static errcode_t create_avtransport_event_xml(char *out, size_t out_size, const dlna_renderer_state &state);
    /*
        功能：创建renderingcontrol_event的xml文本
        参数：
            out：输出缓冲区，xml文本将被写入此处
            out_size：输出缓冲区大小
            state：状态值
        错误码：
            0x01:参数错误
    */
    static errcode_t create_renderingcontrol_event_xml(char *out, size_t out_size, const dlna_renderer_state &state);
    /*
        功能：对xml文本进行基础的转换（注意，tool中的xml处理是解码，这里是编码）
        参数：
            dst：输出缓冲区
            dst_size：输出缓冲区大小
            src：输入字符串
        错误码：
            0x01: 参数错误（如dst为nullptr或dst_size为0或src为nullptr）
            0x02: 输出缓冲区不足，但已经尽力写入了
    */
    static errcode_t xml_escape_basic(char *dst, size_t dst_size, const char *src);
    /*
        功能：调用tool进行解码
        参数：
            text：输入输出字符串
        错误码：
            见 wifi_tool::decode_xml_basic() 的错误码
    */
    static errcode_t xml_unescape_basic(char *text);
    /*
        功能：从xml文本中提取指定标签的值
        参数：
            text：输入xml文本
            tag_name：要提取的标签名，如"Title"
            value_out：输出缓冲区，提取到的值将被写入此处
            value_out_size：输出缓冲区大小
        错误码：
            0x01: 参数错误（如text、tag_name或value_out为nullptr，或value_out_size为0）
    */
    static errcode_t find_xml_tag_value(const char *text, const char *tag_name, char *value_out, size_t value_out_size);

private:
};
} // namespace sed_ws63