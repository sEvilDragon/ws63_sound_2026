#ifndef __WIFI__
#define __WIFI__

extern "C" {
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "wifi_alg.h"
#include "app_init.h"
#include "soc_osal.h"
#include "osal_semaphore.h"
#include "common_def.h"
#include "osal_addr.h"
#include "netinet/in.h"
#include "lwip/sockets.h"
#include "lwip/netifapi.h"
#include "cJSON.h"
}
#include <array>
#include <cstdlib>
#include <cstring>

struct netif;

class wifi {
public:
    wifi();
    static errcode_t wifi_check_dhcp_status(struct netif *netif_p, uint32_t *wait_count, uint32_t max_wait_count);
    // SED：提供重新配网的接口
    static void restart_get_wifi();

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

    // 下面实现softap配网
    static errcode_t softap_start();

    // 实现UDP获得配网需要的网络ssid和密码
    static bool udp();

public:
private:
    // 初始化相关的信号量
    static osal_semaphore scan_done_sem;
    static osal_semaphore connect_done_sem;
    static bool connect_done; // 连接完成标志，初始值为false，连接成功后设置为true

    // 定义一些sta连接的常量
    static constexpr uint8_t wifi_ssid_max_size = 33;                          // SSID最大长度
    static constexpr uint8_t wifi_password_max_size = 65;                      // 密码最大长度
    static constexpr uint8_t wifi_mac_size = 6;                                // MAC地址长度
    static constexpr uint8_t wifi_ifname_max_size = 16;                        // 接口名称最大长度
    static std::array<char, wifi_ssid_max_size> wifi_scan_expect_ssid;         // 期望连接的SSID（网络名称）
    static std::array<char, wifi_password_max_size> wifi_scan_expect_password; // 期望连接的密码
    // 这里不将名称和密码设置为常量，是为了后续可能需要修改这两个值的情况，方便进行赋值和修改
    static constexpr std::array<char, wifi_ifname_max_size + 1> wifi_ifname_sta = {
        "wlan0"};                                                         // STA模式接口名称，默认为"wlan0"
    static constexpr wifi_scan_type_enum wifi_scan_type = WIFI_SSID_SCAN; // 扫描类型，表示扫描基于指定SSID的网络
    static constexpr uint32_t scan_num_max = 64;                          // 扫描结果最大数量
    static constexpr ip_type_stru_enum ip_type = DHCP;                    // IP类型，动态DHCP获取

    // 定义一些softap配网的常量
    static constexpr std::array<char, wifi_ssid_max_size> wifi_softap_ssid = {"2026_sound"};       // SoftAP的SSID
    static constexpr std::array<char, wifi_password_max_size> wifi_softap_password = {"20260320"}; // SoftAP的密码
    static constexpr std::array<char, wifi_ifname_max_size + 1> wifi_ifname_softap = {
        "ap0"}; // SoftAP模式接口名称，默认为"ap0"

    // 设置一些ip地址
    static constexpr std::array<uint8_t, 4> wifi_softap_ip = {192, 168, 43, 1};       // SoftAP的IP地址
    static constexpr std::array<uint8_t, 4> wifi_softap_gw = {192, 168, 43, 1};       // SoftAP的网关地址
    static constexpr std::array<uint8_t, 4> wifi_softap_netmask = {255, 255, 255, 0}; // SoftAP的子网掩码

    // 配置SoftAP的相关参数
    static constexpr wifi_security_enum softap_security_type =
        WIFI_SEC_TYPE_WPA2PSK;                         // SoftAP的安全类型，设置为WPA/WPA2-PSK
    static constexpr int32_t softap_channel_num = 13;  // SoftAP的信道号，设置为13信道
    static constexpr int32_t softap_wifi_psk_type = 0; // SoftAP的PSK类型，设置为0表示默认类型

    // 配置SoftAP的网络参数
    static constexpr uint32_t softap_beacon_interval = 100; // SoftAP的Beacon周期，设置为100ms
    static constexpr uint32_t softap_dtim_period = 2;       // SoftAP的DTIM周期，设置为2
    static constexpr uint32_t softap_gi = 0;                // SoftAP的short GI，设置为0表示默认关闭
    static constexpr protocol_mode_enum softap_protocol_mode = WIFI_MODE_11B_G_N_AX; // SoftAP的协议
    static constexpr uint32_t softap_group_rekey = 86400;  // SoftAP的组播秘钥更新时间，设置为1天（86400秒）
    static constexpr uint32_t softap_hidden_ssid_flag = 1; // SoftAP的SSID隐藏标志，设置为1表示不隐藏SSID

    // 定义一些UDP参数
    static constexpr uint16_t udp_stack_port = 20261;                      // UDP发送端口号
    static constexpr std::array<char, 16> udp_stack_ip = {"192.168.43.1"}; // UDP发送端IP地址
    static constexpr uint16_t udp_buffer = 512;                            // UDP接收缓冲区大小
    static constexpr int32_t udp_sfd = -1;

public:
    // SED：提供更改SSID和密码的接口，方便后续进行修改和赋值
    static void set_wifi_scan_expect_ssid(const std::array<char, wifi_ssid_max_size> &ssid)
    {
        wifi_scan_expect_ssid = ssid;
    }
    static void set_wifi_scan_expect_password(const std::array<char, wifi_password_max_size> &password)
    {
        wifi_scan_expect_password = password;
    }
};

#endif