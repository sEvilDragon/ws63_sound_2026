#pragma once

/*
    sed_ws63::wifi_types.hpp
    该文件定义了最小网络共享类
*/
extern "C" {
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "common_def.h"
}
#include "wifi_tool.hpp"

namespace sed_ws63 {

/*
    softap配置参数
*/
class softapconfig {
public:
    // SED : 这里的SSID和密码在后续应该添加不易失读写存储的功能，允许用户修改并保存配置
    char ssid[33] = "ws63_softap";      // SSID
    char password[65] = "20260502";  // 密码
    static constexpr char ifname[17] = "ap0";    // 接口，通常为ap0

    static constexpr uint8_t ip[4] = {192, 168, 43, 1};        // SoftAP的IP地址
    static constexpr uint8_t gw[4] = {192, 168, 43, 1};        // SoftAP的网关地址
    static constexpr uint8_t netmask[4] = {255, 255, 255, 0}; // SoftAP的子网掩码

    static constexpr wifi_security_enum security_type = WIFI_SEC_TYPE_WPA2PSK; // 安全类型
    static constexpr int32_t channel_num = 13; // 信道
    static constexpr int32_t psk_type = 0;    // PSK类型，设置为0表示默认类型

    static constexpr uint32_t beacon_interval = 100; // 信标间隔，单位为毫秒
    static constexpr uint32_t dtim_period = 2;      // DTIM周期
    static constexpr uint32_t gi = 0;              // GI（Guard Interval）类型，设置为0表示默认关闭
    static constexpr protocol_mode_enum protocol_mode =
        WIFI_MODE_11B_G_N_AX; // softap协议
    static constexpr uint32_t group_rekey = 86400;  // 组播秘钥更新时间，单位为秒，这里设置为1天
    static constexpr uint32_t hidden_ssid_flag = 1; // SSID隐藏标志，设置为1表示不隐藏SSID

private:
public:
    softapconfig();
    ~softapconfig() = default;
    // 开放接口，仅用来不易失存储
    static void set_ssid(const char *new_ssid);
    static void set_password(const char *new_password);
private:
};

/*
    sta连接凭据类，包含连接所需的SSID、密码等信息
*/
class stacredential {
    public:
        char ssid[33] = {0};      // SSID
        char password[65] = {0};  // 密码
    private:
    public:
        stacredential();
        ~stacredential() = default;
        // 开放接口，仅用来不易失存储，不支持修改SSID和密码时的连接功能，仅提供存储功能
        static void set_ssid(const char *new_ssid);
        static void set_password(const char *new_password);
    private:
};

} // namespace sed_ws63