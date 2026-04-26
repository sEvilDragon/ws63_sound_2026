#pragma once

/*
    sed_ws63::softap.hpp
    网络SoftAP功能的实现
*/

extern "C" {
#include "lwip/netifapi.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "common_def.h"
#include "errcode.h"
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "soc_osal.h"
}
#include "wifi_types.hpp"

namespace sed_ws63 {

class softap {
public:
private:
    softapconfig config;     // SoftAP配置对象，包含SSID、密码、IP地址等信息
    bool is_started = false; // 记录SoftAP是否已启动
public:
    explicit softap(const softapconfig &config) : config(config) {};
    ~softap() = default;
    /*
        功能：开启softap
        返回值：ERRCODE_SUCC表示成功，其他值表示失败
        错误码：
            0x01 - SoftAP已启动
            0x02 - SoftAP配置错误
            0x03 - SSID复制错误
            0x04 - 密码复制错误
            0x05 - SoftAP接口未找到
            其他错误码由底层函数返回，具体含义请参考相关函数的文档
    */
    errcode_t init();
    /*
        功能：关闭softap
    */
    void deinit();
    /*
        功能：检查SoftAP是否已启动（就是返回is_started的值）
        返回值：true表示已启动，false表示未启动
    */
    bool is_enabled() const
    {
        return is_started;
    }

private:
};
} // namespace sed_ws63