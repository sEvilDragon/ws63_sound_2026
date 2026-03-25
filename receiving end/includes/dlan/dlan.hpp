#ifndef __DLAN_HPP__
#define __DLAN_HPP__

extern "C" {
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/inet.h"
#include "lwip/igmp.h"
#include "soc_osal.h"
#include "errcode.h"
#include "netinet/in.h"
}
#include <array>
#include <cstdint>

class dlan {
public:
    dlan();
    static void ssdp_and_http_scan();
    static void dlan_stop();
    static void is_ready_set(bool ready)
    {
        is_ready = ready;
    }
    // 提供接口用于外部设置数据处理回调
    using data_process_t = void (*)(const char *data, uint16_t length);
    using data_clear_t = void (*)();
    static void set_data_process_fuction(data_process_t callback);
    static void set_data_clear_fuction(data_clear_t callback);

private:
    static bool ssdp_set();
    static bool http_set();
    static void ssdp_process();
    static void http_process();
    static void ssdp_ip_get();
    static bool ssdp_get_header_value(const char *request, const char *key, char *out, size_t out_size);
    static bool ssdp_send_msearch_reply(const sockaddr_in &client_addr,
                                        socklen_t client_addr_len,
                                        const char *st,
                                        const char *usn_suffix = nullptr);

public:
private:
    // 定义sock
    static bool is_ready;
    static int32_t ssdp_sock;
    static int32_t http_sock;
    // 定义dlan统一编码
    static constexpr uint16_t ssdp_port = 1900;
    static constexpr uint16_t http_port = 49152;
    static constexpr std::array<char, 16> mcast_ip = {"239.255.255.250"};
    // 缓存本地ip
    static std::array<char, 16> local_ip;
    static constexpr int32_t ssdp_timeout = 1800; // SSDP max-age，单位为秒
    static constexpr std::array<char, 128> ssdp_uuid = {"uuid:20260321-1612-2007-0423-a1b2c3d4e5f6"};
    static constexpr int32_t ssdp_config_id = 1;
    static constexpr int32_t ssdp_boot_id = 1;

    // 定义一些xml回复中的常量
    static constexpr float http_xml_version = 1.0;
    static constexpr std::array<char, 32> http_xml_manufacturer = {"sEvil_Dragon"};
    static constexpr std::array<char, 64> http_xml_model_description = {"Audio Device Based on HiSilicon WS63"};
    static constexpr std::array<char, 32> http_xml_name = {"ws63_sound"};

    // 定义一个函数指针类型，用于接收数据的回调
    static data_process_t data_process;
    static data_clear_t data_clear;

    // 构建一个类用来存储当前流媒体的状态
    class media_status {
    public:
        bool has_medio = false;
        bool is_playing = false;
        bool is_paused = false;
        uint32_t duration_seconds = 0;
        uint32_t position_seconds = 0;
        uint8_t volume = 50;
        bool mute = false;
    }now_media_status;
};

#endif