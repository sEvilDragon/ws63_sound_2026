#include "discover_broadcast.hpp"
#include "wifi_task.hpp"

extern "C" {
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/inet.h"
}

#include <cstdio>
#include <cstring>

static constexpr uint16_t k_discover_port = 20262;
static constexpr int k_broadcast_interval_ms = 3000;

static volatile bool s_stop_requested = false;

void discover_broadcast_request_stop(void)
{
    s_stop_requested = true;
}

void discover_broadcast_reset_stop(void)
{
    s_stop_requested = false;
}

void *discover_broadcast_task(void *arg)
{
    (void)arg;
    s_stop_requested = false;
    osal_printk("[Discover] broadcast task started, port=%u interval=%dms\r\n", k_discover_port,
                k_broadcast_interval_ms);

    int32_t sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        osal_printk("[Discover] socket() failed\r\n");
        return nullptr;
    }

    // 启用广播
    int broadcast_enable = 1;
    lwip_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    // 绑定到 INADDR_ANY:0，让 lwip 知道从 wlan0 发出广播
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = 0; // 系统自动分配源端口
    local.sin_addr.s_addr = INADDR_ANY;
    lwip_bind(sock, (sockaddr *)&local, sizeof(local));

    // 目标地址: 255.255.255.255:20262
    sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = lwip_htons(k_discover_port);
    dest.sin_addr.s_addr = IPADDR_BROADCAST;

    while (!s_stop_requested) {
        const char *ip = wifi_get_current_ip();
        const char *name = "FBB音响";
        const char *version = "1.0";

        // 格式: FBB_SOUND_DISCOVER|{"ip":"...","name":"...","version":"..."}
        char msg[256];
        int len = snprintf(msg, sizeof(msg), "FBB_SOUND_DISCOVER|{\"ip\":\"%s\",\"name\":\"%s\",\"version\":\"%s\"}",
                           ip, name, version);

        if (len > 0 && (size_t)len < sizeof(msg)) {
            int sent = lwip_sendto(sock, msg, (size_t)len, 0, (sockaddr *)&dest, sizeof(dest));
            if (sent > 0) {
                // 仅首次或 IP 变更时打印，避免刷屏
                static char last_ip[16] = "";
                if (strcmp(ip, last_ip) != 0) {
                    osal_printk("[Discover] broadcast started, IP=%s\r\n", ip);
                    strncpy(last_ip, ip, sizeof(last_ip) - 1);
                }
            } else {
                osal_printk("[Discover] sendto failed: %d\r\n", sent);
            }
        }

        // 用 sleep 分段实现可中断等待（每次 500ms，共 6 段 = 3 秒）
        for (int i = 0; i < (k_broadcast_interval_ms / 500) && !s_stop_requested; i++) {
            osal_msleep(500);
        }
    }

    lwip_close(sock);
    osal_printk("[Discover] broadcast task exited\r\n");
    return nullptr;
}
