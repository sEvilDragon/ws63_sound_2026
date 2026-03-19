#ifndef __WIFI__
#define __WIFI__

extern "C" {
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "app_init.h"
#include "soc_osal.h"
#include "osal_semaphore.h"
#include "common_def.h"
#include "osal_addr.h"
#include "lwip/netifapi.h"
}
#include <array>
#include <cstdlib>
#include <cstring>

struct netif;

class wifi {
public:
    wifi();
    static errcode_t wifi_check_dhcp_status(struct netif *netif_p, uint32_t *wait_count, uint32_t max_wait_count);

private:
    // 初始化信号量
    static void sem_init();
    // 重置信号量
    static void sem_restart();
    static errcode_t wifi_start_scan();
    static errcode_t wifi_get_network_to_connect(wifi_sta_config_stru *sta_config);
    static void sta_start();
    static errcode_t sta_init();

    // 定义连接状态改变的回调函数
    static void wifi_connection_changed_callback(int32_t state,
                                                 const wifi_linked_info_stru *scan_result,
                                                 int32_t reason_code);
    // 扫描结束的回调
    static void wifi_scan_done_callback(int32_t state, int32_t size);

public:
private:
    // 初始化相关的信号量
    static osal_semaphore scan_done_sem;
    static osal_semaphore connect_done_sem;
    static bool connect_done; // 连接完成标志，初始值为false，连接成功后设置为true

    // 定义一些sta连接的常量
    static constexpr uint8_t wifi_ssid_max_size = 33;                      // SSID最大长度
    static constexpr uint8_t wifi_mac_size = 6;                            // MAC地址长度
    static constexpr uint8_t wifi_ifname_max_size = 16;                    // 接口名称最大长度
    static std::array<char, wifi_ssid_max_size> wifi_scan_expect_ssid;     // 期望连接的SSID（网络名称）
    static std::array<char, wifi_ssid_max_size> wifi_scan_expect_password; // 期望连接的密码
    // 这里不将名称和密码设置为常量，是为了后续可能需要修改这两个值的情况，方便进行赋值和修改
    static constexpr wifi_scan_type_enum wifi_scan_type = WIFI_SSID_SCAN; // 扫描类型，表示扫描基于指定SSID的网络
    static constexpr uint32_t scan_num_max = 64;                          // 扫描结果最大数量
    static constexpr ip_type_stru_enum ip_type = DHCP;                    // IP类型，动态DHCP获取

public:
    // SED：提供更改SSID和密码的接口，方便后续进行修改和赋值
    static void set_wifi_scan_expect_ssid(const std::array<char, wifi_ssid_max_size> &ssid)
    {
        wifi_scan_expect_ssid = ssid;
    }
    static void set_wifi_scan_expect_password(const std::array<char, wifi_ssid_max_size> &password)
    {
        wifi_scan_expect_password = password;
    }
};

#endif