#pragma once

/*
    sed_ws63::sta.hpp
    实现联网功能
*/

extern "C" {
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "soc_osal.h"
#include "osal_semaphore.h"
#include "lwip/netifapi.h"
#include "common_def.h"
#include "errcode.h"
#include "osal_addr.h"
}
#include "wifi_types.hpp"

namespace sed_ws63 {
class sta {
public:
private:
    // 定义连接状态改变的信号量
    osal_semaphore scan_sem_;    // 扫描完成信号量
    osal_semaphore connect_sem_; // 连接完成信号量

    stacredential current_cred_ = {}; // 当前连接的网络信息，包含SSID和密码

    char wifi_ifname[16] = {0}; // WiFi接口名称，后续会通过接口名称获取netif指针进行相关操作

    bool is_connected_ = false;  // 连接状态标志，初始值为false，连接成功后设置为true
    bool auto_reconnect_ = true; // 自动重连标志，默认为true，连接断开后会自动尝试重连
    volatile bool waiting_for_connection_ = false;
    volatile int32_t last_connection_reason_ = 0;

    static constexpr uint32_t max_scan_num = 64;
    static constexpr unsigned int scan_wait_timeout_ms = 10000;
    static constexpr unsigned int connect_wait_timeout_ms = 15000;

    static sta *instance; // 单例实例指针 (用于实现静态回调)

public:
    sta();
    ~sta();
    /*
        功能：连接到指定的WiFi网络
        参数：
            sta最小化配置结构体，包含SSID和密码等必要信息
        返回值：
            ERRCODE_SUCC表示连接成功，ERRCODE_FAIL表示连接失败
    */
    errcode_t sta_connect(const stacredential &cred);
    /*
        功能：断开当前连接的WiFi网络
    */
    void sta_disconnect();
    /*
        功能：自动重连开启
    */
    void enable_auto_reconnect();
    /*
        功能：自动重连关闭
    */
    void disable_auto_reconnect();
    /*
        功能：获取当前连接状态
        返回值：
            true表示已连接，false表示未连接
    */
    bool is_connected() const;
    /*
        功能：获得当前的netif
        返回值：结构体指针
    */
    netif *get_netif();

private:
    /*
        功能：重置信号量
    */
    void reset_semaphores();
    /*
        功能：执行一次连接操作
        参数：
            sta最小化配置结构体，包含SSID和密码等必要信息
        返回值：
            ERRCODE_SUCC表示连接成功
        错误码：
            0x01: sta没有找到接口
            0x02: SSID长度不合法
            0x03: 连接错误，具体错误码查看底层定义
            0x04: 内存分配失败
                0x05: 没有找到对应扫描结果
            其他错误码查看底层定义
    */
    errcode_t do_connect_once(const stacredential &cred);
    // do_scan在do_connect_once中被调用，执行一次扫描操作，获取待连接网络信息
    errcode_t do_scan(const char *ssid);
    // find_and_build_config在do_connect_once中被调用，从扫描结果中获取待连接网络信息并构建连接配置
    errcode_t find_and_build_config(const stacredential &cred, wifi_sta_config_stru *sta_config);
    /*
        以下是两个回调函数，这些函数只用来处理信号量
    */
    static void wifi_connection_changed_callback(int32_t state,
                                                 const wifi_linked_info_stru *scan_result,
                                                 int32_t reason_code);
    static void wifi_scan_done_callback(int32_t state, int32_t size);
};
} // namespace sed_ws63
