# WiFi 模块重构详细教程

## 目录

1. [重构目标与设计原则](#1-重构目标与设计原则)
2. [新目录结构](#2-新目录结构)
3. [模块依赖关系图](#3-模块依赖关系图)
4. [wifi_types 共享类型模块](#4-wifi_types-共享类型模块)
5. [UdpServer 模块](#5-udpserver-模块)
6. [SoftAp 模块](#6-softap-模块)
7. [Sta 模块](#7-sta-模块)
8. [WifiProvisioner 编排层](#8-wifiprovisioner-编排层)
9. [应用层使用示例](#9-应用层使用示例)
10. [CMakeLists.txt 更新方案](#10-cmakeliststxt-更新方案)
11. [原 wifi 类功能对照映射表](#11-原-wifi-类功能对照映射表)
12. [常见问题与注意事项](#12-常见问题与注意事项)

---

## 1. 重构目标与设计原则

### 1.1 现存问题

当前 `wifi` 类存在严重的职责混乱：

| 问题 | 具体表现 |
|---|---|
| **功能耦合** | STA 联网、SoftAP 配网、UDP 接收三件事全部写在同一个类里 |
| **状态共享** | `wifi_scan_expect_ssid` 等静态成员变量被多个功能逻辑同时读写 |
| **不可复用** | `udp()` 方法被硬编码为"接收 WiFi 配网 JSON"，完全无法用于其他 UDP 场景 |
| **扩展困难** | 若要改成 TCP 配网或蓝牙配网，必须大改整个 `wifi` 类 |
| **测试困难** | 不能单独测试 SoftAP 或 STA，因为它们互相依赖 |

### 1.2 设计原则

本次重构遵循以下原则：

- **单一职责**：每个类只做一件事
- **零横向依赖**：`UdpServer`、`SoftAp`、`Sta` 三者之间没有任何 `#include` 关系，它们完全独立
- **通过组合扩展**：`WifiProvisioner` 通过持有这三个类的实例来编排流程，而不是继承
- **配置外置**：所有可变参数通过构造函数或配置结构体传入，类内不硬编码业务数据
- **接口稳定**：模块的 `.hpp` 接口足够通用，使用方式不因内部实现变化而改变

---

## 2. 新目录结构

项目已存在 `includes/wlan/` 库，其内部子目录及 `CMakeLists.txt` 已提前搭好骨架。**不要在 `includes/` 下另建目录**，直接向 `wlan/` 的现有子目录中填写文件即可。

```
includes/wlan/
├── wifi_types/         ← 【填写】跨模块共享的数据类型（SoftApConfig / StaCredential）
│   ├── CMakeLists.txt  （已存在）
│   ├── wifi_types.hpp  （目前仅有框架，本次完善）
│   └── wifi_types.cpp  （目前为空，本次完善）
│
├── wifi_tool/          ← 【已实现，直接复用】字符串工具函数（copy_str 等）
│   ├── wifi_tool.hpp   （已完整实现）
│   └── wifi_tool.cpp
│
├── udp/                ← 【填写】通用 UDP 套接字封装
│   ├── CMakeLists.txt  （需新建）
│   ├── udp_server.hpp
│   └── udp_server.cpp
│
├── softap/             ← 【填写】SoftAP 热点管理
│   ├── CMakeLists.txt  （需新建）
│   ├── softap.hpp
│   └── softap.cpp
│
├── sta/                ← 【填写】STA 联网管理
│   ├── CMakeLists.txt  （需新建）
│   ├── sta.hpp
│   └── sta.cpp
│
└── provision/          ← 【新建目录 + 填写】配网流程编排
    ├── CMakeLists.txt  （需新建，并在 wlan/CMakeLists.txt 中注册）
    ├── wifi_provisioner.hpp
    └── wifi_provisioner.cpp
```

> **注意**：
> - `provision/` 的头文件 `#include` 了其他三个模块，但 `udp/`、`softap/`、`sta/` 彼此之间完全无 `#include` 关系。
> - `wifi_types/` 是所有模块共用的数据类型层，任何模块均可 `#include "wifi_types.hpp"`，不构成横向依赖。
> - 每个 `wlan/` 子目录已被 `wlan/CMakeLists.txt` 添加到头文件搜索路径，因此 `#include` 时直接用文件名，**无需带路径前缀**（如 `#include "udp_server.hpp"` 而非 `#include "wlan/udp/udp_server.hpp"`）。

---

## 3. 模块依赖关系图

```
┌──────────────────────────────────────────────────────────────┐
│                      应用层 (main.cpp)                        │
│  WifiProvisioner provisioner(ap_cfg, "192.168.43.1", 20261); │
│  Sta sta;   provisioner.run(sta);  ← 配网入口                │
└───────────────────────┬──────────────────────────────────────┘
                        │ #include "wifi_provisioner.hpp"
                        ▼
┌───────────────────────────────────────────────────────┐
│         wlan/provision/wifi_provisioner.hpp            │
│   (编排层：组合下面三个模块，完成配网+联网流程)         │
└──────┬──────────────┬─────────────────┬───────────────┘
       │              │                 │
       │ #include     │ #include        │ #include
       │ "udp_server" │ "softap.hpp"    │ "sta.hpp"
       ▼              ▼                 ▼
┌───────────┐  ┌───────────┐  ┌────────────────┐
│ wlan/udp/ │  │wlan/softap│  │  wlan/sta/     │
│ UdpServer │  │ SoftAp    │  │  Sta           │
│  (独立)   │  │  (独立)   │  │  (独立)        │
└───────────┘  └───────────┘  └────────────────┘
       │              │                 │
       └──────────────┴─────────────────┘
                 三者之间无任何 #include 关系

       所有模块均可 #include "wifi_types.hpp" 获取共享类型
       所有模块均可 #include "wifi_tool.hpp"  使用字符串工具
                 ↑ 这两个是横向工具层，不破坏独立性 ↑
```

---

## 4. wifi_types 模块（共享数据类型）

### 4.1 职责说明

`wifi_types` 是所有模块共用的**纯数据类型层**。它不包含任何业务逻辑，只定义跨模块传递数据时用到的结构体和工厂函数。

- **`SoftApConfig`**：描述一个 SoftAP 热点的全部参数
- **`StaCredential`**：描述待连接的 WiFi 网络（SSID + 密码）
- **`make_default_softap_config()`**：工厂函数，返回填了常用默认值的 `SoftApConfig`

把这些类型集中放在 `wifi_types/` 的好处是：`SoftAp`、`Sta`、`WifiProvisioner` 和应用层都 `#include "wifi_types.hpp"`，不需要互相 `#include` 对方的头文件，保持零横向依赖。

### 4.2 `wlan/wifi_types/wifi_types.hpp`

```cpp
#pragma once

extern "C" {
#include "wifi_hotspot.h"       // wifi_security_enum
#include "wifi_hotspot_config.h" // protocol_mode_enum
#include "common_def.h"
}

#include <cstdint>

namespace sed_ws63 {

/**
 * @brief SoftAP 完整配置参数
 *
 * 推荐使用 make_default_softap_config() 获取填好默认值的实例，
 * 然后按需覆盖个别字段。
 */
struct SoftApConfig {
    // ---- 基本参数 ----
    char ssid[33];      ///< 热点 SSID，最长 32 字节
    char password[65];  ///< 热点密码，最长 64 字节
    char ifname[17];    ///< 网络接口名称，通常为 "ap0"

    // ---- 网络参数 ----
    uint8_t ip[4];       ///< 热点 IP 地址，如 {192, 168, 43, 1}
    uint8_t netmask[4];  ///< 子网掩码，如 {255, 255, 255, 0}
    uint8_t gateway[4];  ///< 网关，通常与 ip 相同

    // ---- RF 参数 ----
    uint8_t            channel;        ///< 信道号 (1~13)
    wifi_security_enum security_type;  ///< 安全类型

    // ---- 高级参数（通常使用默认值即可） ----
    uint32_t           beacon_interval;   ///< Beacon 周期(ms)，默认 100
    uint32_t           dtim_period;       ///< DTIM 周期，默认 2
    uint32_t           gi;                ///< Short GI，0 = 关闭
    protocol_mode_enum protocol_mode;     ///< 协议模式
    uint32_t           group_rekey;       ///< 组密钥更新周期(s)，默认 86400
    uint32_t           hidden_ssid_flag;  ///< 1 = 广播 SSID（不隐藏）
};

/**
 * @brief STA 连接凭据（SSID + 密码）
 *
 * 这是 STA 模块与外部世界之间的唯一数据契约。
 * WifiProvisioner 解析配网报文后填充此结构体，再传给 Sta::connect()。
 */
struct StaCredential {
    char ssid[33];      ///< WiFi 网络名称，最长 32 字节
    char password[65];  ///< WiFi 密码，最长 64 字节
};

/**
 * @brief 创建填充了常用默认值的 SoftApConfig
 *
 * 默认值：
 *   - ifname  : "ap0"，IP : 192.168.43.1 / 255.255.255.0
 *   - channel : 13，security : WPA2-PSK，protocol : 11B/G/N/AX
 *
 * @param ssid     热点 SSID
 * @param password 热点密码（至少 8 位）
 */
SoftApConfig make_default_softap_config(const char *ssid, const char *password);

}  // namespace sed_ws63
```

### 4.3 `wlan/wifi_types/wifi_types.cpp`

```cpp
#include "wifi_types.hpp"
#include "wifi_tool.hpp"   // sed_ws63::wifi_tool::copy_str()

extern "C" {
#include "soc_osal.h"
}

#include <cstring>

namespace sed_ws63 {

SoftApConfig make_default_softap_config(const char *ssid, const char *password)
{
    SoftApConfig cfg;
    (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));

    // 使用项目已有的 wifi_tool::copy_str() 进行安全字符串拷贝
    (void)wifi_tool::copy_str(cfg.ssid,     sizeof(cfg.ssid),     ssid);
    (void)wifi_tool::copy_str(cfg.password, sizeof(cfg.password), password);
    (void)wifi_tool::copy_str(cfg.ifname,   sizeof(cfg.ifname),   "ap0");

    cfg.ip[0] = 192; cfg.ip[1] = 168; cfg.ip[2] = 43; cfg.ip[3] = 1;
    cfg.gateway[0] = 192; cfg.gateway[1] = 168;
    cfg.gateway[2] = 43;  cfg.gateway[3] = 1;
    cfg.netmask[0] = 255; cfg.netmask[1] = 255;
    cfg.netmask[2] = 255; cfg.netmask[3] = 0;

    cfg.channel          = 13;
    cfg.security_type    = WIFI_SEC_TYPE_WPA2PSK;
    cfg.beacon_interval  = 100;
    cfg.dtim_period      = 2;
    cfg.gi               = 0;
    cfg.protocol_mode    = WIFI_MODE_11B_G_N_AX;
    cfg.group_rekey      = 86400;
    cfg.hidden_ssid_flag = 1;

    return cfg;
}

}  // namespace sed_ws63
```

### 4.4 `wlan/wifi_types/CMakeLists.txt`（已存在，内容正确，无需修改）

```cmake
set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/wifi_types.cpp"
    PARENT_SCOPE)
```

---

## 5. UdpServer 模块

### 4.1 职责说明

`UdpServer` 是一个**通用 UDP 接收套接字封装**。它只负责：
- 创建并绑定 UDP 套接字
- 阻塞等待并接收一条数据报
- 返回原始字节，**不做任何业务解析**

它完全不知道"WiFi 配网"是什么概念，将来可以用于接收任意 UDP 数据（传感器数据、控制指令、OTA 包等）。

### 5.2 `wlan/udp/udp_server.hpp`

```cpp
#ifndef __UDP_SERVER_HPP__
#define __UDP_SERVER_HPP__

extern "C" {
#include "lwip/sockets.h"
#include "netinet/in.h"
#include "common_def.h"
}

/**
 * @brief 通用 UDP 接收套接字封装
 *
 * 该类与 WiFi 配网、STA/SoftAP 等业务逻辑完全无关。
 * 它只提供"创建套接字 → 绑定端口 → 收发数据"的原子能力。
 *
 * 使用示例（独立使用，不需要 WifiProvisioner）：
 *
 *   UdpServer server;
 *   if (server.bind("192.168.43.1", 8888) == ERRCODE_SUCC) {
 *       char buf[256];
 *       struct sockaddr_in from;
 *       int32_t len = server.recv_from(buf, sizeof(buf), &from);
 *       if (len > 0) {
 *           // 处理 buf 中的数据 ...
 *           // 回复发送方
 *           server.send_to(inet_ntoa(from.sin_addr),
 *                          lwip_ntohs(from.sin_port),
 *                          "ACK", 3);
 *       }
 *       server.close_socket();
 *   }
 */
class UdpServer {
public:
    UdpServer();
    ~UdpServer();

    /**
     * @brief 创建套接字并绑定到指定本地 IP 和端口
     *
     * @param bind_ip  本地 IP 地址字符串，如 "192.168.43.1" 或 "0.0.0.0"
     * @param port     监听端口号
     * @return ERRCODE_SUCC 绑定成功；ERRCODE_FAIL 失败（会自动关闭套接字）
     */
    errcode_t bind(const char *bind_ip, uint16_t port);

    /**
     * @brief 阻塞接收一条 UDP 数据报
     *
     * 接收成功后会在 buf[len] 处写入 '\0'，保证字符串安全。
     * buf_len 必须比期望最大数据长度至少大 1（用于存放 '\0'）。
     *
     * @param buf       接收缓冲区（调用方分配）
     * @param buf_len   缓冲区总长度（含结尾 '\0' 的预留位）
     * @param from_addr 可选输出参数，获取发送方的地址和端口；不需要时传 nullptr
     * @return 实际接收到的字节数（不含结尾 '\0'）；< 0 表示失败
     */
    int32_t recv_from(char *buf, uint16_t buf_len,
                      struct sockaddr_in *from_addr = nullptr);

    /**
     * @brief 向指定目标发送一条 UDP 数据报
     *
     * 调用前必须已通过 bind() 创建过套接字（sfd_ >= 0）。
     * 典型用途：在 recv_from() 收到数据后，向发送方回复响应包。
     *
     * @param dest_ip    目标 IP 地址字符串，如 "192.168.43.100"
     * @param dest_port  目标端口号
     * @param buf        待发送数据缓冲区
     * @param len        待发送字节数
     * @return 实际发送的字节数；< 0 表示失败
     */
    int32_t send_to(const char *dest_ip, uint16_t dest_port,
                    const void *buf, uint16_t len);

    /**
     * @brief 关闭套接字并释放资源
     * 析构函数会自动调用，也可以手动提前关闭。
     */
    void close_socket();

    /**
     * @brief 查询套接字是否已绑定（即是否可以调用 recv_from / send_to）
     */
    bool is_bound() const;

private:
    int32_t sfd_;  ///< 套接字文件描述符，-1 表示未创建或已关闭
};

#endif  // __UDP_SERVER_HPP__
```

### 5.3 `wlan/udp/udp_server.cpp`

```cpp
#include "udp_server.hpp"  // wlan/ 各子目录均已加入头文件搜索路径，直接引用文件名

extern "C" {
#include "soc_osal.h"
#include "lwip/netifapi.h"
}

// ============================================================
// 构造 / 析构
// ============================================================

UdpServer::UdpServer() : sfd_(-1) {}

UdpServer::~UdpServer()
{
    close_socket();
}

// ============================================================
// 公共接口实现
// ============================================================

errcode_t UdpServer::bind(const char *bind_ip, uint16_t port)
{
    // 防止重复 bind
    if (sfd_ >= 0) {
        close_socket();
    }

    sfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd_ < 0) {
        osal_printk("[UdpServer] 创建套接字失败\n");
        return ERRCODE_FAIL;
    }

    struct sockaddr_in srv_addr;
    (void)memset_s(&srv_addr, sizeof(srv_addr), 0, sizeof(srv_addr));
    srv_addr.sin_family      = AF_INET;
    srv_addr.sin_addr.s_addr = inet_addr(bind_ip);
    srv_addr.sin_port        = lwip_htons(port);

    if (lwip_bind(sfd_, reinterpret_cast<struct sockaddr *>(&srv_addr),
                  sizeof(srv_addr)) != 0) {
        osal_printk("[UdpServer] 绑定失败，IP: %s  Port: %u\n", bind_ip, port);
        lwip_close(sfd_);
        sfd_ = -1;
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCC;
}

int32_t UdpServer::recv_from(char *buf, uint16_t buf_len,
                              struct sockaddr_in *from_addr)
{
    if (sfd_ < 0) {
        osal_printk("[UdpServer] 套接字未绑定，无法接收\n");
        return -1;
    }
    if (buf == nullptr || buf_len == 0) {
        return -1;
    }

    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);
    (void)memset_s(&sender, sizeof(sender), 0, sizeof(sender));

    // buf_len - 1：预留最后一字节给 '\0'
    int32_t ret = lwip_recvfrom(sfd_, buf,
                                static_cast<size_t>(buf_len - 1), 0,
                                reinterpret_cast<struct sockaddr *>(&sender),
                                &sender_len);
    if (ret < 0) {
        osal_printk("[UdpServer] 接收数据失败\n");
        return -1;
    }

    buf[ret] = '\0';  // 保证字符串安全

    if (from_addr != nullptr) {
        *from_addr = sender;
    }

    return ret;
}

int32_t UdpServer::send_to(const char *dest_ip, uint16_t dest_port,
                            const void *buf, uint16_t len)
{
    if (sfd_ < 0) {
        osal_printk("[UdpServer] 套接字未绑定，无法发送\n");
        return -1;
    }
    if (buf == nullptr || len == 0) {
        return -1;
    }

    struct sockaddr_in dest_addr;
    (void)memset_s(&dest_addr, sizeof(dest_addr), 0, sizeof(dest_addr));
    dest_addr.sin_family      = AF_INET;
    dest_addr.sin_addr.s_addr = inet_addr(dest_ip);
    dest_addr.sin_port        = lwip_htons(dest_port);

    int32_t ret = lwip_sendto(sfd_, buf, static_cast<size_t>(len), 0,
                              reinterpret_cast<struct sockaddr *>(&dest_addr),
                              sizeof(dest_addr));
    if (ret < 0) {
        osal_printk("[UdpServer] 发送失败，目标: %s:%u\n", dest_ip, dest_port);
        return -1;
    }

    return ret;
}

void UdpServer::close_socket()
{
    if (sfd_ >= 0) {
        lwip_close(sfd_);
        sfd_ = -1;
    }
}

bool UdpServer::is_bound() const
{
    return sfd_ >= 0;
}
```

### 5.4 `wlan/udp/CMakeLists.txt`（需新建）

```cmake
set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/udp_server.cpp"
    PARENT_SCOPE)
```

---

## 6. SoftAp 模块

### 6.1 职责说明

`SoftAp` 负责管理 SoftAP 热点的**完整生命周期**：启用（含 IP 配置和 DHCP 服务器）与关闭。

它完全不知道"谁会连接这个热点"或"连上之后要做什么"。它只负责让热点跑起来。

**配置类型来自 `wifi_types`**：`SoftAp` 接受 `sed_ws63::SoftApConfig` 作为构造参数，该类型定义在 `wifi_types.hpp` 中，与本模块无硬绑定。

### 6.2 `wlan/softap/softap.hpp`

```cpp
#pragma once

#include "wifi_types.hpp"   // sed_ws63::SoftApConfig（来自 wlan/wifi_types/）

extern "C" {
#include "lwip/netifapi.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "common_def.h"
}

namespace sed_ws63 {

/**
 * @brief SoftAP 热点管理类
 *
 * 使用示例：
 *
 *   using namespace sed_ws63;
 *   SoftApConfig cfg = make_default_softap_config("MyHotspot", "12345678");
 *   SoftAp ap(cfg);
 *   ap.enable();
 *   // ... 等待客户端完成业务 ...
 *   ap.disable();
 */
class SoftAp {
public:
    explicit SoftAp(const SoftApConfig &config);

    /**
     * @brief 启用 SoftAP：配置 RF → 绑定接口 IP → 启动 DHCP 服务器
     * @return ERRCODE_SUCC 成功；ERRCODE_FAIL 失败（已自动回滚）
     */
    errcode_t enable();

    /** @brief 关闭 SoftAP（未启用时静默返回） */
    void disable();

    bool is_enabled() const;

private:
    SoftApConfig config_;
    bool         enabled_ = false;
};

}  // namespace sed_ws63
```

### 6.3 `wlan/softap/softap.cpp`

```cpp
#include "softap.hpp"   // wlan/ 各子目录均已加入头文件搜索路径，直接引用文件名

extern "C" {
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "soc_osal.h"
}

namespace sed_ws63 {

SoftAp::SoftAp(const SoftApConfig &config)
    : config_(config), enabled_(false)
{
}
}

// ============================================================
// enable()
// ============================================================

errcode_t SoftAp::enable()
{
    if (enabled_) {
        return ERRCODE_SUCC;
    }

    // ---- 1. 填充基本配置 ----
    softap_config_stru hapd_conf;
    (void)memset_s(&hapd_conf, sizeof(hapd_conf), 0, sizeof(hapd_conf));

    uint32_t ssid_len = strlen(config_.ssid);
    uint32_t pwd_len  = strlen(config_.password);

    if (memcpy_s(hapd_conf.ssid, sizeof(hapd_conf.ssid),
                 config_.ssid, ssid_len) != 0) {
        osal_printk("[SoftAp] 复制 SSID 失败\n");
        return ERRCODE_FAIL;
    }
    if (memcpy_s(hapd_conf.pre_shared_key, sizeof(hapd_conf.pre_shared_key),
                 config_.password, pwd_len) != 0) {
        osal_printk("[SoftAp] 复制密码失败\n");
        return ERRCODE_FAIL;
    }
    hapd_conf.security_type = config_.security_type;
    hapd_conf.channel_num   = static_cast<int32_t>(config_.channel);
    hapd_conf.wifi_psk_type = 0;

    // ---- 2. 填充高级配置 ----
    softap_config_advance_stru adv_cfg;
    (void)memset_s(&adv_cfg, sizeof(adv_cfg), 0, sizeof(adv_cfg));
    adv_cfg.beacon_interval  = config_.beacon_interval;
    adv_cfg.dtim_period      = config_.dtim_period;
    adv_cfg.gi               = config_.gi;
    adv_cfg.protocol_mode    = config_.protocol_mode;
    adv_cfg.group_rekey      = config_.group_rekey;
    adv_cfg.hidden_ssid_flag = config_.hidden_ssid_flag;

    errcode_t ret = wifi_set_softap_config_advance(&adv_cfg);
    if (ret != 0) {
        osal_printk("[SoftAp] 设置高级配置失败，错误码: %u\n", ret);
        return ret;
    }

    // ---- 3. 启用 SoftAP ----
    ret = wifi_softap_enable(&hapd_conf);
    if (ret != 0) {
        osal_printk("[SoftAp] 使能失败，错误码: %u\n", ret);
        return ret;
    }

    // ---- 4. 配置接口 IP ----
    struct netif *netif_p = netif_find(config_.ifname);
    if (netif_p == nullptr) {
        osal_printk("[SoftAp] 未找到接口: %s\n", config_.ifname);
        (void)wifi_softap_disable();
        return ERRCODE_FAIL;
    }

    ip4_addr_t ip, netmask, gw;
    IP4_ADDR(&ip,
             config_.ip[0], config_.ip[1], config_.ip[2], config_.ip[3]);
    IP4_ADDR(&netmask,
             config_.netmask[0], config_.netmask[1],
             config_.netmask[2], config_.netmask[3]);
    IP4_ADDR(&gw,
             config_.gateway[0], config_.gateway[1],
             config_.gateway[2], config_.gateway[3]);

    ret = netifapi_netif_set_addr(netif_p, &ip, &netmask, &gw);
    if (ret != 0) {
        osal_printk("[SoftAp] 设置接口地址失败，错误码: %u\n", ret);
        (void)wifi_softap_disable();
        return ret;
    }

    // ---- 5. 启动 DHCP 服务器 ----
    ret = netifapi_dhcps_start(netif_p, nullptr, 0);
    if (ret != 0) {
        osal_printk("[SoftAp] 启动 DHCP 服务器失败，错误码: %u\n", ret);
        (void)wifi_softap_disable();
        return ret;
    }

    enabled_ = true;
    osal_printk("[SoftAp] 已启动，SSID: %s  信道: %u\n",
                config_.ssid, config_.channel);
    return ERRCODE_SUCC;
}

// ============================================================
// disable()
// ============================================================

void SoftAp::disable()
{
    if (enabled_) {
        (void)wifi_softap_disable();
        enabled_ = false;
        osal_printk("[SoftAp] 已关闭\n");
    }
}

bool SoftAp::is_enabled() const
{
    return enabled_;
}

}  // namespace sed_ws63
```

### 6.4 `wlan/softap/CMakeLists.txt`（需新建）

```cmake
set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/softap.cpp"
    PARENT_SCOPE)
```

---

## 7. Sta 模块

### 7.1 职责说明

`Sta` 负责管理 STA（Station）模式的完整连接流程：
- 使能 STA
- 定向扫描目标 SSID
- 获取扫描结果并构造连接配置
- 发起连接
- 等待连接成功信号
- 启动 DHCP 获取 IP

调用方只需传入 `StaCredential`（SSID + 密码），其余细节全部封装在内部。

### 7.2 静态回调的处理策略

WS63 SDK 的 WiFi 事件回调必须是 C 风格的**静态函数**（函数指针）。要在静态回调中访问实例成员，标准的嵌入式 C++ 做法是：

```
在 .cpp 文件中声明一个文件作用域的静态指针 s_instance_
                  │
                  ▼
构造函数：s_instance_ = this
                  │
                  ▼
静态回调：if (s_instance_) { s_instance_->实例方法(); }
```

这在"整个系统只有一个 STA 实例"的嵌入式环境中是完全安全且标准的做法。

### 7.3 `wlan/sta/sta.hpp`

```cpp
#pragma once

#include "wifi_types.hpp"   // sed_ws63::StaCredential（来自 wlan/wifi_types/）

extern "C" {
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "soc_osal.h"
#include "osal_semaphore.h"
#include "lwip/netifapi.h"
#include "common_def.h"
}

struct netif;

namespace sed_ws63 {

/**
 * @brief STA 模式 WiFi 管理类
 *
 * 独立使用示例（不经过配网流程，直接连接已知网络）：
 *
 *   StaCredential cred;
 *   wifi_tool::copy_str(cred.ssid,     sizeof(cred.ssid),     "MyWiFi");
 *   wifi_tool::copy_str(cred.password, sizeof(cred.password), "password");
 *
 *   Sta sta;
 *   sta.enable_auto_reconnect(true);
 *   errcode_t ret = sta.connect(cred);
 *   if (ret == ERRCODE_SUCC) {
 *       // 已连接，netif_p = sta.get_netif();
 *   }
 */
class Sta {
public:
    Sta();
    ~Sta();

    /**
     * @brief 连接到指定 WiFi 网络（阻塞，直到连接+DHCP成功或失败）
     *
     * 每次调用都会重置内部信号量状态，可以重复调用（先断开再连接）。
     *
     * @param cred  包含 SSID 和密码的凭据
     * @return ERRCODE_SUCC 连接成功并获取到 IP；ERRCODE_FAIL 失败
     */
    errcode_t connect(const StaCredential &cred);

    /**
     * @brief 断开当前连接并禁用 STA（同时关闭自动重连）
     */
    void disconnect();

    /**
     * @brief 设置断线后是否自动重连（默认：开启）
     *
     * 开启后，WiFi 断线时 Sta 会自动使用上次连接的凭据重新发起连接。
     * 若需要外部控制重连时机（例如先重新配网再连），请关闭此选项。
     */
    void enable_auto_reconnect(bool enable);

    /**
     * @brief 查询当前是否已连接到网络
     */
    bool is_connected() const;

    /**
     * @brief 获取 STA 对应的网络接口指针
     *
     * 可用于获取 IP 地址等信息：
     *   struct netif *nif = sta.get_netif();
     *   osal_printk("IP: %s\n", ip4addr_ntoa(&nif->ip_addr));
     *
     * @return 指向 netif 的指针；未连接或未找到接口时返回 nullptr
     */
    struct netif *get_netif();

private:
    // ---- 连接流程内部函数 ----

    /** 执行一次完整的"扫描→找AP→连接→DHCP"流程 */
    errcode_t do_connect_once(const StaCredential &cred);

    /** 向指定 SSID 发起定向扫描 */
    errcode_t do_scan(const char *ssid);

    /** 从扫描结果中找到目标 AP 并构造 wifi_sta_config_stru */
    errcode_t find_and_build_config(const StaCredential &cred,
                                    wifi_sta_config_stru *out);

    /** 重置两个信号量（销毁后重新初始化），用于每次 connect() 调用前的清理 */
    void reset_semaphores();

    // ---- SDK 事件回调（必须为静态，通过 s_instance_ 桥接到实例） ----

    static void on_connection_changed(int32_t state,
                                      const wifi_linked_info_stru *info,
                                      int32_t reason_code);
    static void on_scan_done(int32_t state, int32_t size);

    // ---- 成员变量 ----

    osal_semaphore  scan_sem_;         ///< 扫描完成信号量
    osal_semaphore  connect_sem_;      ///< 连接完成信号量
    StaCredential   current_cred_;     ///< 当前凭据，供自动重连使用
    char            ifname_[17];       ///< STA 接口名称，默认 "wlan0"
    bool            connected_      = false;
    bool            auto_reconnect_ = true;

    static constexpr uint32_t k_scan_num_max = 64;

    /** 文件级单例指针，在 sta.cpp 中定义 */
    static Sta *s_instance_;
};

}  // namespace sed_ws63
```

### 7.4 `wlan/sta/sta.cpp`

```cpp
#include "sta.hpp"   // wlan/ 各子目录均已加入头文件搜索路径，直接引用文件名

extern "C" {
#include "soc_osal.h"
#include "osal_addr.h"
}

namespace sed_ws63 {

// ============================================================
// 文件作用域单例指针（回调桥接用）
// ============================================================

Sta *Sta::s_instance_ = nullptr;

// ============================================================
// 构造 / 析构
// ============================================================

Sta::Sta()
    : connected_(false), auto_reconnect_(true)
{
    s_instance_ = this;

    (void)memset_s(ifname_,        sizeof(ifname_),        0, sizeof(ifname_));
    (void)memset_s(&current_cred_, sizeof(current_cred_),  0, sizeof(current_cred_));
    wifi_tool::copy_str(ifname_, sizeof(ifname_), "wlan0");  // 使用 wifi_tool 复制字符串

    osal_sem_init(&scan_sem_,    0);
    osal_sem_init(&connect_sem_, 0);

    // 注册 WiFi 事件回调（整个系统只注册一次）
    wifi_event_stru cb;
    (void)memset_s(&cb, sizeof(cb), 0, sizeof(cb));
    cb.wifi_event_connection_changed = on_connection_changed;
    cb.wifi_event_scan_state_changed = on_scan_done;

    errcode_t ret = wifi_register_event_cb(&cb);
    if (ret != 0) {
        osal_printk("[Sta] 注册事件回调失败，错误码: %u\n", ret);
    }
}

Sta::~Sta()
{
    disconnect();
    osal_sem_destroy(&scan_sem_);
    osal_sem_destroy(&connect_sem_);
    if (s_instance_ == this) {
        s_instance_ = nullptr;
    }
}

// ============================================================
// 公共接口
// ============================================================

errcode_t Sta::connect(const StaCredential &cred)
{
    // 保存凭据，供自动重连使用
    (void)memcpy_s(&current_cred_, sizeof(current_cred_),
                   &cred, sizeof(cred));

    connected_ = false;
    reset_semaphores();

    return do_connect_once(cred);
}

void Sta::disconnect()
{
    auto_reconnect_ = false;
    (void)wifi_sta_disable();
    connected_ = false;
}

void Sta::enable_auto_reconnect(bool enable)
{
    auto_reconnect_ = enable;
}

bool Sta::is_connected() const
{
    return connected_;
}

struct netif *Sta::get_netif()
{
    return netifapi_netif_find_by_name(ifname_);
}

// ============================================================
// 内部流程
// ============================================================

void Sta::reset_semaphores()
{
    osal_sem_destroy(&scan_sem_);
    osal_sem_destroy(&connect_sem_);
    osal_sem_init(&scan_sem_,    0);
    osal_sem_init(&connect_sem_, 0);
}

errcode_t Sta::do_connect_once(const StaCredential &cred)
{
    // ---- 1. 使能 STA（含一次自动恢复） ----
    errcode_t ret = wifi_sta_enable();
    if (ret != 0) {
        osal_printk("[Sta] STA 使能失败(%u)，尝试恢复\n", ret);
        (void)wifi_sta_disable();
        osal_msleep(100);
        ret = wifi_sta_enable();
        if (ret != 0) {
            osal_printk("[Sta] 恢复后仍失败，错误码: %u\n", ret);
            return ERRCODE_FAIL;
        }
    }

    // ---- 2. 配置 SDK 自动重连策略 ----
    wifi_sta_set_reconnect_policy(1, 10, 10, 100);

    // ---- 3. 发起定向扫描 ----
    ret = do_scan(cred.ssid);
    if (ret != 0) {
        return ret;
    }
    osal_sem_down(&scan_sem_);  // 等待扫描完成

    // ---- 4. 从扫描结果构造连接配置 ----
    wifi_sta_config_stru sta_cfg;
    (void)memset_s(&sta_cfg, sizeof(sta_cfg), 0, sizeof(sta_cfg));
    ret = find_and_build_config(cred, &sta_cfg);
    if (ret != 0) {
        return ret;
    }

    // ---- 5. 发起连接 ----
    ret = wifi_sta_connect(&sta_cfg);
    if (ret != 0) {
        osal_printk("[Sta] 连接请求失败，错误码: %u\n", ret);
        return ret;
    }
    osal_sem_down(&connect_sem_);  // 等待连接成功回调

    // ---- 6. 启动 DHCP ----
    struct netif *netif_p = get_netif();
    if (netif_p == nullptr) {
        osal_printk("[Sta] 未找到接口: %s\n", ifname_);
        return ERRCODE_FAIL;
    }
    ret = netifapi_dhcp_start(netif_p);
    if (ret != 0) {
        osal_printk("[Sta] DHCP 启动失败，错误码: %u\n", ret);
        return ret;
    }

    connected_ = true;
    osal_printk("[Sta] 已连接并获取 IP，SSID: %s\n", cred.ssid);
    return ERRCODE_SUCC;
}

errcode_t Sta::do_scan(const char *ssid)
{
    wifi_scan_params_stru params;
    (void)memset_s(&params, sizeof(params), 0, sizeof(params));

    uint32_t ssid_len = strlen(ssid);
    if (ssid_len == 0 || ssid_len >= sizeof(params.ssid)) {
        osal_printk("[Sta] SSID 长度非法: %u\n", ssid_len);
        return ERRCODE_FAIL;
    }
    if (memcpy_s(params.ssid, sizeof(params.ssid),
                 ssid, ssid_len + 1) != 0) {
        return ERRCODE_FAIL;
    }
    params.ssid_len  = static_cast<uint8_t>(ssid_len);
    params.scan_type = WIFI_SSID_SCAN;

    errcode_t ret = wifi_sta_scan_advance(&params);
    if (ret != 0) {
        osal_printk("[Sta] 发起扫描失败，错误码: %u\n", ret);
    }
    return ret;
}

errcode_t Sta::find_and_build_config(const StaCredential &cred,
                                     wifi_sta_config_stru *out)
{
    uint32_t num = k_scan_num_max;
    int32_t  buf_size = static_cast<int32_t>(sizeof(wifi_scan_info_stru) * num);

    wifi_scan_info_stru *results = static_cast<wifi_scan_info_stru *>(
        osal_kmalloc(buf_size, OSAL_GFP_ATOMIC));
    if (results == nullptr) {
        osal_printk("[Sta] 分配扫描结果内存失败\n");
        return ERRCODE_FAIL;
    }
    (void)memset_s(results, buf_size, 0, buf_size);

    errcode_t ret = wifi_sta_get_scan_info(results, &num);
    if (ret != 0) {
        osal_kfree(results);
        osal_printk("[Sta] 获取扫描结果失败，错误码: %u\n", ret);
        return ret;
    }

    // 在扫描结果中查找目标 SSID
    bool     found    = false;
    uint32_t match_idx = 0;
    uint32_t ssid_len  = strlen(cred.ssid);

    for (uint32_t i = 0; i < num; i++) {
        if (strlen(results[i].ssid) == ssid_len &&
            memcmp(results[i].ssid, cred.ssid, ssid_len) == 0) {
            found     = true;
            match_idx = i;
            break;
        }
    }

    if (!found) {
        osal_kfree(results);
        osal_printk("[Sta] 未找到目标 AP: %s\n", cred.ssid);
        return ERRCODE_FAIL;
    }

    // 填充连接配置
    uint32_t pwd_len = strlen(cred.password);
    bool ok = (memcpy_s(out->ssid, sizeof(out->ssid),
                        cred.ssid, ssid_len) == 0) &&
              (memcpy_s(out->bssid, sizeof(out->bssid),
                        results[match_idx].bssid, 6) == 0) &&
              (memcpy_s(out->pre_shared_key, sizeof(out->pre_shared_key),
                        cred.password, pwd_len) == 0);

    if (!ok) {
        osal_kfree(results);
        osal_printk("[Sta] 填充连接配置失败\n");
        return ERRCODE_FAIL;
    }

    out->security_type = results[match_idx].security_type;
    out->ip_type       = DHCP;

    osal_kfree(results);
    return ERRCODE_SUCC;
}

// ============================================================
// 静态回调（SDK 调用，通过 s_instance_ 转发到实例方法）
// ============================================================

void Sta::on_connection_changed(int32_t state,
                                const wifi_linked_info_stru *info,
                                int32_t reason_code)
{
    if (s_instance_ == nullptr) {
        return;
    }

    if (state == 1) {
        // 连接成功：释放 connect_sem_，让 do_connect_once 继续执行
        osal_printk("[Sta] 连接成功，SSID: %s\n",
                    (info != nullptr) ? info->ssid : "unknown");
        osal_sem_up(&s_instance_->connect_sem_);
    } else {
        // 连接断开
        osal_printk("[Sta] 连接断开，reason: %d\n", reason_code);
        s_instance_->connected_ = false;

        if (s_instance_->auto_reconnect_) {
            osal_printk("[Sta] 自动重连中...\n");
            (void)wifi_sta_disable();
            osal_msleep(300);
            s_instance_->reset_semaphores();
            // 使用上次保存的凭据重新发起连接
            (void)s_instance_->do_connect_once(s_instance_->current_cred_);
        }
    }
}

void Sta::on_scan_done(int32_t state, int32_t size)
{
    (void)state;
    (void)size;
    if (s_instance_ != nullptr) {
        osal_sem_up(&s_instance_->scan_sem_);
    }
}
```

### 7.5 `wlan/sta/CMakeLists.txt`（需新建）

```cmake
set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/sta.cpp"
    PARENT_SCOPE)
```

---

## 8. WifiProvisioner 编排层

### 8.1 职责说明

`WifiProvisioner` 是一个**可选的编排类**，它将 `SoftAp`、`UdpServer`、`Sta` 三个模块组合成一个完整的"配网 + 联网"流程。

**它不是必须的**。如果你在代码中已经保存了上次的 SSID/密码（例如存在 Flash/NV 中），可以直接使用 `Sta::connect()` 跳过配网流程。

**JSON 解析集中在此处**：`UdpServer` 只返回原始字节，JSON 解析由 `WifiProvisioner` 负责，保持各模块职责清晰。

### 8.2 `wlan/provision/wifi_provisioner.hpp`

```cpp
#pragma once

#include "softap.hpp"      // sed_ws63::SoftAp（路径已由 CMakeLists 加入搜索路径）
#include "udp_server.hpp"  // UdpServer
#include "sta.hpp"         // sed_ws63::Sta
#include "wifi_types.hpp"  // sed_ws63::SoftApConfig, sed_ws63::StaCredential

/**
 * @brief WiFi 配网流程编排类
 *
 * 完整配网流程（按顺序）：
 *   1. 启动 SoftAP 热点，等待手机连入
 *   2. 通过 UDP 接收手机发来的 JSON 报文（包含 ssid 和 password 字段）
 *   3. 关闭 SoftAP，等待射频稳定
 *   4. 以收到的凭据调用 Sta::connect() 连接正式网络
 *
 * 期望收到的 UDP JSON 格式：
 *   { "ssid": "MyNetwork", "password": "MyPassword" }
 *
 * 使用示例：
 *
 *   using namespace sed_ws63;
 *   SoftApConfig ap_cfg = make_default_softap_config("2026_sound", "20260320");
 *   WifiProvisioner provisioner(ap_cfg, "192.168.43.1", 20261);
 *
 *   Sta sta;
 *   sta.enable_auto_reconnect(true);
 *
 *   // run() 会阻塞直到联网成功
 *   while (provisioner.run(sta) != ERRCODE_SUCC) {
 *       osal_msleep(500);
 *   }
 */
class WifiProvisioner {
public:
    /**
     * @param ap_cfg      SoftAP 热点配置（使用 make_default_softap_config 生成）
     * @param udp_bind_ip UDP 绑定的本地 IP（通常与 ap_cfg.ip 相同）
     * @param udp_port    UDP 监听端口
     */
    WifiProvisioner(const SoftApConfig &ap_cfg,
                    const char *udp_bind_ip,
                    uint16_t udp_port);

    /**
     * @brief 执行完整配网流程（阻塞）
     *
     * @param sta  外部创建的 Sta 实例，配网成功后处于已连接状态
     * @return ERRCODE_SUCC 全流程成功；ERRCODE_FAIL 任意步骤失败
     */
    errcode_t run(Sta &sta);

private:
    /**
     * @brief 通过 UDP 接收并解析一条含 SSID/密码的 JSON 报文
     * @param out  输出：解析出的凭据
     * @return ERRCODE_SUCC 成功
     */
    errcode_t receive_credential(StaCredential *out);

    SoftApConfig ap_cfg_;
    char         udp_bind_ip_[16];
    uint16_t     udp_port_;

    static constexpr uint16_t k_udp_buf_size = 512;
};

}  // namespace sed_ws63
```

### 8.3 `wlan/provision/wifi_provisioner.cpp`

```cpp
#include "wifi_provisioner.hpp"  // wlan/ 各子目录均已加入头文件搜索路径

extern "C" {
#include "soc_osal.h"
#include "cJSON.h"
}

namespace sed_ws63 {

// ============================================================
// 构造函数
// ============================================================

WifiProvisioner::WifiProvisioner(const SoftApConfig &ap_cfg,
                                 const char *udp_bind_ip,
                                 uint16_t udp_port)
    : ap_cfg_(ap_cfg), udp_port_(udp_port)
{
    // 使用 wifi_tool::copy_str() 进行安全字符串拷贝
    (void)wifi_tool::copy_str(udp_bind_ip_, sizeof(udp_bind_ip_), udp_bind_ip);
}

// ============================================================
// run() —— 编排完整配网流程
// ============================================================

errcode_t WifiProvisioner::run(Sta &sta)
{
    // ---- 步骤 1：启动 SoftAP ----
    SoftAp ap(ap_cfg_);
    errcode_t ret = ap.enable();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[Provisioner] SoftAP 启动失败\n");
        return ERRCODE_FAIL;
    }

    // ---- 步骤 2：UDP 阻塞接收凭据 ----
    StaCredential cred;
    (void)memset_s(&cred, sizeof(cred), 0, sizeof(cred));
    ret = receive_credential(&cred);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[Provisioner] 获取配网凭据失败\n");
        ap.disable();
        return ERRCODE_FAIL;
    }

    // ---- 步骤 3：关闭 SoftAP，等待射频稳定 ----
    ap.disable();
    osal_msleep(1000);

    // ---- 步骤 4：STA 连接正式网络 ----
    ret = sta.connect(cred);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[Provisioner] STA 连接失败，SSID: %s\n", cred.ssid);
        return ERRCODE_FAIL;
    }

    osal_printk("[Provisioner] 配网完成！已连接到: %s\n", cred.ssid);
    return ERRCODE_SUCC;
}

// ============================================================
// receive_credential() —— UDP 接收 + JSON 解析
// ============================================================

errcode_t WifiProvisioner::receive_credential(StaCredential *out)
{
    UdpServer udp;

    if (udp.bind(udp_bind_ip_, udp_port_) != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }

    char buf[k_udp_buf_size];
    (void)memset_s(buf, sizeof(buf), 0, sizeof(buf));

    int32_t len = udp.recv_from(buf, k_udp_buf_size);
    udp.close_socket();  // 接收一条即关闭，UdpServer 析构也会保证关闭

    if (len <= 0) {
        osal_printk("[Provisioner] UDP 未收到数据\n");
        return ERRCODE_FAIL;
    }

    // ---- 解析 JSON ----
    cJSON *json = cJSON_Parse(buf);
    if (json == nullptr) {
        osal_printk("[Provisioner] JSON 解析失败，原始数据: %s\n", buf);
        return ERRCODE_FAIL;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
    cJSON *pwd_item  = cJSON_GetObjectItem(json, "password");

    if (ssid_item == nullptr || pwd_item == nullptr ||
        ssid_item->valuestring == nullptr || pwd_item->valuestring == nullptr) {
        osal_printk("[Provisioner] JSON 缺少 ssid 或 password 字段\n");
        cJSON_Delete(json);
        return ERRCODE_FAIL;
    }

    // ---- 长度检查并复制 ----
    size_t ssid_len = strlen(ssid_item->valuestring);
    size_t pwd_len  = strlen(pwd_item->valuestring);

    if (ssid_len == 0 || ssid_len >= sizeof(out->ssid) ||
        pwd_len  == 0 || pwd_len  >= sizeof(out->password)) {
        osal_printk("[Provisioner] SSID 或密码长度非法\n");
        cJSON_Delete(json);
        return ERRCODE_FAIL;
    }

    // 使用项目已有的 wifi_tool::copy_str() 复制 SSID 和密码
    if (wifi_tool::copy_str(out->ssid, sizeof(out->ssid),
                             ssid_item->valuestring) != ERRCODE_SUCC ||
        wifi_tool::copy_str(out->password, sizeof(out->password),
                             pwd_item->valuestring) != ERRCODE_SUCC) {
        osal_printk("[Provisioner] 凭据复制失败\n");
        cJSON_Delete(json);
        return ERRCODE_FAIL;
    }

    cJSON_Delete(json);
    osal_printk("[Provisioner] 收到凭据，SSID: %s\n", out->ssid);
    return ERRCODE_SUCC;
}

}  // namespace sed_ws63
```

### 8.4 `wlan/provision/CMakeLists.txt`（需新建）

```cmake
set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/wifi_provisioner.cpp"
    PARENT_SCOPE)
```

---

## 9. 应用层使用示例

这里展示如何用新的模块体系替换原来的 `wifi` 类，同时实现原来所有的功能。

### 9.1 场景一：完整配网 + 联网（替换原 `wifi::wifi()` 构造函数逻辑）

```cpp
// main.cpp（或对应的任务函数）
#include "sta.hpp"
#include "wifi_provisioner.hpp"
#include "wifi_types.hpp"

using namespace sed_ws63;

// 全局 Sta 实例（或在任务栈中分配）
static Sta g_sta;

void wifi_task(void *args)
{
    // 等待 WiFi 硬件底层初始化完成
    while (wifi_is_wifi_inited() == 0) {
        osal_msleep(100);
    }

    // 开启自动重连
    g_sta.enable_auto_reconnect(true);

    // 构建 SoftAP 配置
    SoftApConfig ap_cfg = make_default_softap_config("2026_sound", "20260320");

    // 创建配网编排器
    WifiProvisioner provisioner(ap_cfg, "192.168.43.1", 20261);

    // 执行配网 + 联网（失败则重试）
    while (provisioner.run(g_sta) != ERRCODE_SUCC) {
        osal_printk("[App] 配网失败，1秒后重试...\n");
        osal_msleep(1000);
    }

    // WiFi 已就绪，通知后续业务
    // g_sta.is_connected() == true
    // g_sta.get_netif()    != nullptr
}
```

### 9.2 场景二：跳过配网，直接用已知凭据联网

```cpp
#include "sta.hpp"
#include "wifi_types.hpp"

using namespace sed_ws63;

void wifi_direct_connect_task(void *args)
{
    while (wifi_is_wifi_inited() == 0) {
        osal_msleep(100);
    }

    Sta sta;
    sta.enable_auto_reconnect(true);

    StaCredential cred{};
    // 使用项目已有的 wifi_tool::copy_str()，无需 memcpy_s
    wifi_tool::copy_str(cred.ssid,     sizeof(cred.ssid),     "OPPO Find X8 972E");
    wifi_tool::copy_str(cred.password, sizeof(cred.password), "mytc4386");

    while (sta.connect(cred) != ERRCODE_SUCC) {
        osal_msleep(500);
    }
}
```

### 9.3 场景三：重新配网（替换原 `wifi::restart_get_wifi()`）

```cpp
void restart_provisioning(Sta &sta)
{
    // 关闭自动重连，否则断线时会自动用旧凭据重连，干扰配网流程
    sta.enable_auto_reconnect(false);
    sta.disconnect();
    osal_msleep(100);

    // 重新走配网流程
    SoftApConfig ap_cfg = make_default_softap_config("2026_sound", "20260320");
    WifiProvisioner provisioner(ap_cfg, "192.168.43.1", 20261);

    while (provisioner.run(sta) != ERRCODE_SUCC) {
        osal_msleep(500);
    }

    // 配网成功后重新开启自动重连
    sta.enable_auto_reconnect(true);
}
```

### 9.4 场景四：单独使用 UdpServer 接收任意数据（展示扩展性）

```cpp
// 例如：在已联网后，通过 UDP 接收 OTA 控制指令
void udp_control_task(void *args)
{
    UdpServer server;
    if (server.bind("0.0.0.0", 9999) != ERRCODE_SUCC) {
        return;
    }

    char buf[256];
    while (true) {
        struct sockaddr_in from;
        int32_t len = server.recv_from(buf, sizeof(buf), &from);
        if (len > 0) {
            osal_printk("[CtrlTask] 收到指令: %s\n", buf);
            // 处理业务...
        }
    }
}
```

---

## 10. CMakeLists.txt 更新方案

### 10.1 各模块 CMakeLists.txt（已在各节展示，汇总如下）

各模块的 `CMakeLists.txt` 已在第 5～8 节逐一给出，按提示新建即可。`wlan/CMakeLists.txt` **已经包含**了 `udp`、`softap`、`sta` 的 `add_subdirectory_if_exist` 和 `PUBLIC_HEADER` 条目，**不需要手动修改这三项**。

### 10.2 向 `wlan/CMakeLists.txt` 中添加 `provision` 子目录

打开 `includes/wlan/CMakeLists.txt`，在已有的 `add_subdirectory_if_exist` 列表末尾**增加一行**：

```cmake
add_subdirectory_if_exist(provision)
```

同时在 `PUBLIC_HEADER` 列表中增加：

```cmake
"${CMAKE_CURRENT_SOURCE_DIR}/provision"
```

完整示例（只需新增带注释的两行，其余保持不变）：

```cmake
# 已有内容（不要修改）
add_subdirectory_if_exist(wifi_tool)
add_subdirectory_if_exist(wifi_types)
add_subdirectory_if_exist(udp)
add_subdirectory_if_exist(softap)
add_subdirectory_if_exist(sta)
# ---- 新增 ----
add_subdirectory_if_exist(provision)

set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}/wifi_tool"
    "${CMAKE_CURRENT_SOURCE_DIR}/wifi_types"
    "${CMAKE_CURRENT_SOURCE_DIR}/udp"
    "${CMAKE_CURRENT_SOURCE_DIR}/softap"
    "${CMAKE_CURRENT_SOURCE_DIR}/sta"
    # ---- 新增 ----
    "${CMAKE_CURRENT_SOURCE_DIR}/provision"
    PARENT_SCOPE)
```

### 10.3 修复头文件搜索路径传播

**重要问题**：`includes/CMakeLists.txt` 目前只将 `SOURCES` 传递到上一级作用域（`PARENT_SCOPE`），但**没有传递 `PUBLIC_HEADER`**。这意味着 `wlan/` 各子目录设置的 `PUBLIC_HEADER` 无法到达最终的 `include_directories`，导致编译时找不到头文件。

打开 `main/receiving end/includes/CMakeLists.txt`，找到末尾的 `PARENT_SCOPE` 传递语句，确认或添加如下一行：

```cmake
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
```

添加后，`wlan/` 各子目录的头文件路径就能正确传递到编译系统，之后即可使用**扁平 include 风格**（项目统一约定）：

```cpp
#include "udp_server.hpp"    // 不需要写 "udp/udp_server.hpp"
#include "softap.hpp"
#include "sta.hpp"
#include "wifi_provisioner.hpp"
#include "wifi_types.hpp"
```

### 10.4 旧 `includes/wifi/` 目录的迁移

新模块测试通过后，可以：

1. 注释掉（或删除）`includes/CMakeLists.txt` 中的 `add_subdirectory_if_exist(wifi)`
2. 删除 `includes/wifi/wifi.cpp` 和 `includes/wifi/wifi.hpp`
3. 将所有仍在使用 `#include "wifi.hpp"` 的文件改为引入对应的新头文件

---

## 11. 原 wifi 类功能对照映射表

| 原 `wifi` 类的功能 | 新模块中的位置 |
|---|---|
| `wifi::wifi()` 构造函数（整个初始化流程） | 应用层任务函数 + `WifiProvisioner::run()` |
| `wifi::restart_get_wifi()` | 应用层调用 `sta.disconnect()` + `provisioner.run(sta)` |
| `wifi::sta_start()` | `Sta::connect()` 内部的重试逻辑 |
| `wifi::sta_init()` | `Sta::do_connect_once()` |
| `wifi::wifi_start_scan()` | `Sta::do_scan()` |
| `wifi::wifi_get_network_to_connect()` | `Sta::find_and_build_config()` |
| `wifi::sem_init()` / `wifi::sem_restart()` | `Sta::reset_semaphores()` |
| `wifi::wifi_connection_changed_callback()` | `Sta::on_connection_changed()` |
| `wifi::wifi_scan_done_callback()` | `Sta::on_scan_done()` |
| `wifi::softap_start()` | `SoftAp::enable()` |
| `wifi::udp()` | `UdpServer::bind()` + `UdpServer::recv_from()` + `WifiProvisioner::receive_credential()` 中的 JSON 解析 |
| `wifi::wifi_check_dhcp_status()` | 目前未直接使用，如需检查 DHCP 状态可在应用层调用 `sta.get_netif()` 后检查 `ip_addr` 字段 |
| `wifi::set_wifi_scan_expect_ssid()` | 直接构造 `StaCredential` 并传给 `sta.connect()` 即可，无需全局 setter |
| `wifi::set_wifi_scan_expect_password()` | 同上 |
| `wifi::is_ready` 标志 | `sta.is_connected()` |

---

## 12. 常见问题与注意事项

### Q1：`wifi_is_wifi_inited()` 应该在哪里调用？

在**应用层任务**的最开始调用，放在创建任何 WiFi 相关对象之前：

```cpp
while (wifi_is_wifi_inited() == 0) {
    osal_msleep(100);
}
// 之后再创建 Sta、SoftAp 等对象
```

这行代码不属于任何一个模块，因为它是系统级的等待，应该由应用层负责。

---

### Q2：`Sta` 的静态 `s_instance_` 能不能支持多实例？

**不能**，也不需要。WS63 硬件只有一个 WiFi 射频，只能有一个 STA 实例。静态单例指针的设计与硬件约束完全匹配。如果你的代码里出现了创建两个 `Sta` 对象的情况，第二个对象的构造会覆盖 `s_instance_`，导致第一个对象的回调失效——这是一个设计错误，应在代码审查阶段发现。

---

### Q3：`on_connection_changed` 回调中调用 `do_connect_once()` 是阻塞调用，安全吗？

这与原代码的行为**完全一致**（原代码在回调中调用了 `sta_start()`）。在 LiteOS 中，WiFi 事件回调运行在一个 WiFi 事件任务的上下文中，该上下文有足够的栈空间支持阻塞调用。如果遇到栈溢出问题（通常表现为莫名其妙的 Hardfault），可以将重连逻辑改为：在回调中设置一个标志位，由一个独立的重连任务检测并处理。

---

### Q4：编译时提示找不到 `make_default_softap_config` 函数？

该函数定义在 `softap.cpp` 中，声明在 `softap.hpp` 中（注意它是一个**自由函数**，不是类的成员）。确保：
1. `wlan/softap/CMakeLists.txt` 已将 `softap.cpp` 加入 `SOURCES`
2. 你的 `#include "softap.hpp"` 路径正确（扁平风格）
3. `includes/CMakeLists.txt` 已正确传递 `PUBLIC_HEADER`（见第 10.3 节）

---

### Q5：如果不需要配网，只想用硬编码的 SSID 和密码，还需要 `provision/` 模块吗？

**不需要**。`provision/` 模块是完全可选的。只需：

```cpp
#include "sta.hpp"
// 不需要 #include "wifi_provisioner.hpp"
// 不需要 #include "softap.hpp"
// 不需要 #include "udp_server.hpp"
```

同时也不需要将 `softap/`、`udp/`、`provision/` 的 `.cpp` 文件加入编译。这就是模块化设计的价值：**用什么才编译什么**。

---

### Q6：`StaCredential` 中的 SSID/密码为什么不用 `std::array`？

原代码中 `wifi_scan_expect_ssid` 用了 `std::array<char, 33>`，但在传递凭据这个场景下，普通的 `char[]` 加上 `memcpy_s` 更安全、更直观，且避免了 `std::array` 在不同上下文中隐式 `data()` 调用容易遗漏的问题。如果你的代码风格统一使用 `std::array`，将 `StaCredential` 改为：

```cpp
struct StaCredential {
    std::array<char, 33> ssid     = {};
    std::array<char, 65> password = {};
};
```

同时对应修改 `Sta` 内部所有使用 `cred.ssid` 和 `cred.password` 的地方改为 `.data()` 即可。

---

*文档版本：v1.1 | 日期：2026-04-25*
