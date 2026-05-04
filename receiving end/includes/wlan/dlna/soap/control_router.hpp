#pragma once

/*
    sed_ws63:control_router.hpp
    处理DLNA控制点SOAP请求的类，负责解析请求并调用相应的处理函数
*/

#include "wifi_tool.hpp"
#include "xml/xml.hpp"
#include "dlna_types/dlna_types.hpp"

namespace sed_ws63 {

class subscription_manager;
class playback_bridge;

class control_router {
public:
private:
public:
    /*
        统一处理
    */
    errcode_t handle_request(const char *request,
                             const char *local_ip,
                             uint16_t http_port,
                             const char *udn,
                             dlna_renderer_state *state,
                             subscription_manager *subs,
                             playback_bridge *bridge,
                             dlna_control_response *out);

private:
    /*
        功能：
            处理GET请求，解析请求并调用相应的处理函数
        参数：
            - request: HTTP请求的内容
            - local_ip: 本地IP地址
            - http_port: HTTP服务器的端口
            - udn: 设备的UDN
            - state: 当前渲染器的状态
            - out: 输出参数，包含HTTP响应的内容
        错误码：
            0x01: 无效的请求
    */
    errcode_t handle_get(const char *request,
                         const char *local_ip,
                         uint16_t http_port,
                         const char *udn,
                         dlna_renderer_state *state,
                         dlna_control_response *out);
    /*
        功能：
            处理UNSUBSCRIBE请求，解析请求并调用相应的处理函数
        参数：
            - request: HTTP请求的内容
            - subs: 订阅管理器实例
            - out: 输出参数，包含HTTP响应的内容
        错误码：
            0x01: 无效的请求
    */
    errcode_t handle_unsubscribe(const char *request, subscription_manager *subs, dlna_control_response *out);
    /*
        功能：
            处理SUBSCRIBE请求，解析请求并调用相应的处理函数
        参数：
            - request: HTTP请求的内容
            - subs: 订阅管理器实例
            - out: 输出参数，包含HTTP响应的内容
    */
    errcode_t handle_subscribe(const char *request, subscription_manager *subs, dlna_control_response *out);
    /*
        功能：
            处理POST请求，解析SOAP动作并调用相应的处理函数
        参数：
            - request: HTTP请求的内容
            - state: 当前渲染器的状态
            - subs: 订阅管理器实例
            - bridge: 播放控制桥实例
            - out: 输出参数，包含HTTP响应的内容
        错误码：
            0x01: 无效的请求
     */
    errcode_t handle_post(const char *request,
                          dlna_renderer_state *state,
                          subscription_manager *subs,
                          playback_bridge *bridge,
                          dlna_control_response *out);
};
} // namespace sed_ws63