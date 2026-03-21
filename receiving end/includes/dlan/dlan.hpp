#ifndef __DLAN_HPP__
#define __DLAN_HPP__

extern "C" {
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/inet.h"
#include "soc_osal.h"
#include "errcode.h"
#include "netinet/in.h"
}
#include <array>

class dlan {
public:
    dlan();
    static void ssdp_and_http_scan();
    static void dlan_stop();
    static void is_ready_set(bool ready) { is_ready = ready; }

private:
    static void ssdp_set();
    static void http_set();
    static void ssdp_process();
    static void http_process();
    static void ssdp_ip_get();

public:
private:
    // 定义sock
    static bool is_ready;
    static uint32_t ssdp_sock;
    static uint32_t http_sock;
    // 定义dlan统一编码
    static constexpr uint16_t ssdp_port = 1900;
    static constexpr uint16_t http_port = 49152;
    static constexpr std::array<char, 16> mcast_ip = {"239.255.255.250"};
    // 缓存本地ip
    static std::array<char, 16> local_ip;
    static constexpr int32_t ssdp_timeout = 5000; // SSDP套接字的超时时间，单位为毫秒
    static constexpr std::array<char, 128> ssdp_uuid = {"uuid:20260321-1612-2007-0423-a1b2c3d4e5f6"};
};

#endif