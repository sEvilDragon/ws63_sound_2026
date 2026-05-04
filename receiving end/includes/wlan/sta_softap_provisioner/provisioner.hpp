#pragma once

/*
    sed_ws63::provisioner.hpp
    实现热点配网
*/

extern "C" {
#include "cJSON.h"
}
#include "sta.hpp"
#include "softap.hpp"
#include "udp.hpp"
#include "wifi_types.hpp"

namespace sed_ws63 {

class softap_provisioner {
public:
private:
    static constexpr uint16_t udp_buffer_size_ = 512; // UDP接收缓冲区大小
public:
    softap_provisioner() = default;
    ~softap_provisioner() = default;

    /*
        功能：
            启动配网流程，创建SoftAP并监听UDP数据包
        参数：
            sta-外部创建的sta对象，用于连接SoftAP；sta对象需要在调用start_provisioning前创建并初始化好
        错误码：
            0x01: 启动SoftAP失败，返回错误码
            0x02: 接收凭据失败，返回错误码
            0x03: sta连接失败，返回错误码
    */
    errcode_t run(sta &sta_obj, softapconfig config = {});

private:
    /*
        功能：
            通过udp接收并解析一条json报文
        参数：
            解析出的SSID和密码会通过参数返回
            地址和端口
        错误码：
            0x04: UDP绑定失败，返回错误码
            0x05: udp接收数据失败，返回错误码
            0x06: json解析失败，返回错误码
            0x07: json格式错误，缺少ssid或password字段，返回错误码
            0x08: SSID或密码长度不合法，返回错误码
    */
    errcode_t receive_credentials(stacredential *out, const char *udp_bind_ip, uint16_t udp_bind_port);
};
} // namespace sed_ws63
