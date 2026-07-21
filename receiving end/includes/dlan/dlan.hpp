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
#include "http_utils.hpp"
#include <array>
#include <cstdint>

class dlan {
public:
    dlan();
    // Return true only when the renderer accepted the URI and prepared a
    // playable session.  This lets SetAVTransportURI report relay failures
    // instead of acknowledging a request that Play cannot honor.
    using media_set_uri_handler = bool (*)(const char *uri);
    using media_play_handler = bool (*)(const char *uri);
    using media_pause_handler = void (*)();
    using media_stop_handler = void (*)();
    using media_seek_handler = void (*)(uint32_t seconds_target);

    static void register_media_set_uri_handler(media_set_uri_handler handler);
    static void register_media_play_handler(media_play_handler handler);
    static void register_media_pause_handler(media_pause_handler handler);
    static void register_media_stop_handler(media_stop_handler handler);
    static void register_media_seek_handler(media_seek_handler handler);

    static void ssdp_and_http_scan();
    static void dlan_stop();
    static void request_stop();
    static void reset_stop();
    static volatile bool s_stop_requested;
    static void is_ready_set(bool ready)
    {
        is_ready = ready;
    }

private:
    static bool ssdp_set();
    static bool http_set();
    static void ssdp_process();
    static void http_process();
    static void ssdp_ip_get();
    static bool ssdp_send_msearch_reply(const sockaddr_in &client_addr,
                                        socklen_t client_addr_len,
                                        const char *st,
                                        const char *usn_suffix = nullptr);

public:
    // 声明全局传输状态，实现在 dlan.cpp 中
    static std::array<char, 32> g_transport_state;

private:
    static media_set_uri_handler media_set_uri_handler_func;
    static media_play_handler media_play_handler_func;
    static media_pause_handler media_pause_handler_func;
    static media_stop_handler media_stop_handler_func;
    static media_seek_handler media_seek_handler_func;

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
    } now_media_status;
};

#endif
