# DLNA 模块重构详细教程

## 目录

1. [重构目标与设计原则](#1-重构目标与设计原则)
2. [当前实现中需要立即拆开的内容](#2-当前实现中需要立即拆开的内容)
3. [新目录结构](#3-新目录结构)
4. [模块依赖关系图](#4-模块依赖关系图)
5. [dlna_types 共享类型模块](#5-dlna_types-共享类型模块)
6. [tcp 模块](#6-tcp-模块)
7. [xml 模块](#7-xml-模块)
8. [event 模块](#8-event-模块)
9. [soap 控制路由模块](#9-soap-控制路由模块)
10. [ssdp 模块](#10-ssdp-模块)
11. [playback_bridge 模块](#11-playback_bridge-模块)
12. [media_renderer 编排层](#12-media_renderer-编排层)
13. [应用层使用示例](#13-应用层使用示例)
14. [play 播放链路模块](#14-play-播放链路模块)
15. [CMakeLists.txt 更新方案](#15-cmakeliststxt-更新方案)
16. [原 dlan 与旧播放链路功能对照映射表](#16-原-dlan-与旧播放链路功能对照映射表)
17. [建议删除或下沉的旧代码](#17-建议删除或下沉的旧代码)
18. [常见问题与注意事项](#18-常见问题与注意事项)

---

## 1. 重构目标与设计原则

### 1.1 本次重构的真正目标

你现在要重写的不是“再写一个更大的 dlan.cpp”，而是把当前混在一起的四类职责拆开：

- SSDP 发现
- HTTP/SOAP 控制面
- XML 文本构造与解析
- 播放控制桥接

同时，你给出的网络播放逻辑也暴露出另一个事实：

- DLNA 控制面和音频数据面本来就是两条链路，不应该继续硬塞在同一个类里
- 当前仓库里自研的 `minimp3.cpp/minimp3.hpp` 不应该再作为未来架构保留，它要被一个新的 `play/` 文件夹整体替换
- 真正需要保留的只有第三方解码库 `miniMP3/minimp3.h`，它以后只作为 `play/mp3_decoder.cpp` 的内部依赖存在

所以这次设计的核心不是“功能更多”，而是“边界更清楚”。

### 1.2 设计原则

本次 DLNA 重构遵循以下原则：

- 单一职责：一个模块只做一类事
- XML 独立：XML 转义、标签提取、SOAP/XML 文本构造单独放到 xml 子模块
- TCP 独立：主动 TCP 连接和被动监听从 DLNA 协议逻辑里拆出
- 控制面与数据面解耦：DLNA 只发播放命令，不直接解码或拉流
- 命名去歧义：工程里不再继续用 `minimp3` 这个类名指代自研播放器，`miniMP3` 只表示第三方解码库
- 直接替换而不是兼容套壳：旧 `minimp3.cpp/minimp3.hpp` 不再保留为核心路径，而是被 `play/` 正式取代
- 渐进迁移的是协议边界，不是旧播放器接口：DLNA 动作可以分阶段补，但播放数据面要一次性从旧自研 `minimp3` 迁出
- 保持构建可靠：当前工程已经把 includes/wlan/dlna 和 includes/wlan/tcp 加进头文件搜索路径，因此首版示例统一采用“根目录 + 子路径” include 风格，避免一开始就卡在 PUBLIC_HEADER 传播上

### 1.3 本次重构的非目标

你已经明确说了，当前 DLNA 不是完全体，不需要这次顺手补协议覆盖面。因此第一阶段不做下面这些事：

- 不新增 Seek、Next、Previous、SetPlayMode 等动作
- 不把当前“伪暂停”强行升级成真正断点暂停恢复
- 不新增 HTTPS、chunked、AAC、FLAC 等数据面能力
- 不为 ConnectionManager 补齐你当前没有用到的复杂场景
- 不再为了兼容旧自研 `minimp3` 静态接口而额外保留一层过渡壳

先把边界拆干净，比补动作更重要。

---

## 2. 当前实现中需要立即拆开的内容

### 2.1 现有 dlan.cpp 的核心问题

当前 dlan.cpp 同时承担了下面这些职责：

| 问题 | 具体表现 |
|---|---|
| 控制面过载 | SSDP、HTTP accept、SOAPAction 分发、SUBSCRIBE/UNSUBSCRIBE、NOTIFY 全在一个文件里 |
| 文本层混杂 | XML 转义、SOAP body 拼接、设备描述 XML 构造和 socket 发送混在一起 |
| 状态散落 | `g_current_uri`、`g_avt_callback`、`g_rc_callback`、`g_transport_state` 等全是文件作用域静态状态 |
| 网络细节泄漏 | NOTIFY 主动 TCP 请求直接写在 dlan.cpp 内部，和协议状态机耦合 |
| 回调绑定粗糙 | `register_media_*_handler()` 只是几个全局静态函数指针，没有实例边界 |
| 可删代码夹杂在热路径附近 | 调试辅助、遗留状态、未再使用的探测函数和真正协议逻辑混在一起，维护成本高 |

### 2.2 现有自研 minimp3.cpp 的核心问题

这里说的 `minimp3.cpp` 指的是你自己写的那个播放器根文件，不是第三方 `miniMP3/minimp3.h` 解码库。

当前这份自研 `minimp3.cpp` 也承担了过多职责：

| 问题 | 具体表现 |
|---|---|
| 播放控制和解码耦合 | `http_set_url()`、`http_get_url()`、`http_stop()`、`http_clear_url()` 属于播放会话控制，不是解码器职责 |
| 网络拉流和解码耦合 | socket 建连、HTTP 请求、响应头解析、ICY 过滤和 MP3 解码全在 `stream_mp3_to_iis()` |
| 探测逻辑分散 | 格式探测、ID3 跳过、sync 重定位混杂在主循环内部和匿名工具函数中 |
| 节奏控制外泄 | 当前真正的 PCM 队列闭环节奏控制写在 `wifi_task.cpp` 里，不属于 WiFi 任务 |
| 静态全局状态过多 | `current_url`、`is_playing`、`is_url_ready` 这些状态本质上应由播放会话持有 |

### 2.3 当前实现里明确可以处理的冗余项

下面这些内容已经可以明确标记为“应删除”或“应下沉”，不需要再继续挂在 dlan/minimp3 根文件上：

| 位置 | 处理建议 | 原因 |
|---|---|---|
| `dlan.cpp::stream_probe_once()` | 直接删除 | 当前无调用路径；即便恢复调用，也不应放在 Play 热路径里，因为额外 GET 可能竞争签名 URL |
| `dlan.hpp::media_status` 和 `now_media_status` | 直接删除 | 当前无任何引用，是纯遗留状态 |
| `dlan.cpp::send_http_notify_request()` | 下沉到 `event/notify_sender.cpp` | 这是事件发送器，不是 renderer 主体逻辑 |
| `dlan.cpp::send_http_soap_response()` / `send_http_soap_fault_response()` | 下沉到 `soap/control_router.cpp` 内部辅助函数或单独 response_writer | 这是 HTTP 响应写入，不是 DLNA 根类状态 |
| `dlan.cpp::xml_escape_basic()` | 移到 `dlna/xml/xml_text.cpp` | XML 文本处理必须独立 |
| `wifi_tool::decode_xml_basic()` | 对外只通过 xml 模块暴露 | 调用点属于 XML 处理，不应让控制路由直接碰底层文本工具 |
| `wifi_tool::find_html_tag_value()` | 对外只通过 xml 模块暴露 | 同上 |
| `includes/miniMP3/minimp3.hpp` 与 `includes/miniMP3/minimp3.cpp` | 直接删除并由 `includes/play/` 取代 | 这是旧自研播放器入口，名称还会和第三方 `miniMP3` 库冲突 |
| `minimp3.cpp::parse_http_status_code_from_header()` | 当前未使用，可删除或移入未来 `http_stream_reader.cpp` | 不应留在解码根文件里占位 |
| `minimp3.cpp::probe_mp3_frame_from_buffer()` | 当前未使用，可删除或移入未来 `format_probe.cpp` | 它属于预探测工具，不属于主解码类接口 |
| `wifi_task.cpp::push_pcm_with_closed_loop()` | 下沉到播放数据面模块 | 这是 PCM 节奏控制，不属于 WiFi 任务 |

### 2.4 哪些旧逻辑不该删，而是该“换位置”

不是所有旧代码都多余，下面这些逻辑是对的，只是位置不对：

- `is_mp3_candidate_from_uri_or_metadata()`：应保留，但移到 AVTransport 路由或播放策略层
- `ssdp:all` 需要多条响应：应保留，但移到 `ssdp_service`
- SUBSCRIBE 首次回复后立即发初始 NOTIFY：应保留，但改为 `subscription_manager + notify_sender + media_renderer` 协作完成
- `SetAVTransportURI` 与 `Play` 解耦：必须保留，这是正确行为
- `CurrentURI` 的 XML 实体反解码：必须保留，但放进 xml 模块
- 外部 `miniMP3/minimp3.h`：必须保留，但只允许 `play/mp3_decoder.cpp` 直接 include

---

## 3. 新目录结构

### 3.1 这次建议的目录安排

当前仓库已经预留了这两个空目录：

- `includes/wlan/tcp/`
- `includes/wlan/dlna/`

但这次还必须额外新增一个顶层 `includes/play/`。原因不是“文件多了”，而是边界已经变了：

- `play` 不属于 DLNA 子模块，它是通用播放数据面
- 旧自研 `minimp3.cpp/minimp3.hpp` 的 URL 状态、HTTP 拉流、解码驱动、播放控制都要整体迁过去
- 第三方 `miniMP3/minimp3.h` 依然保留在 `includes/miniMP3/miniMP3/`，但它只作为 `play/mp3_decoder.cpp` 的内部依赖

因此这次推荐的目录安排是：

- TCP 放到 `includes/wlan/tcp/`
- XML 作为 `includes/wlan/dlna/xml/` 子目录
- 播放数据面放到新的 `includes/play/`

这样既实现了独立模块，又不会把通用播放器错误地塞回 DLNA 目录里。

### 3.2 推荐结构

```text
includes/
├── play/                             ← 【本次新增】正式替换旧自研 minimp3
│   ├── CMakeLists.txt
│   ├── play_service.hpp
│   ├── play_service.cpp
│   ├── playback_session.hpp
│   ├── playback_session.cpp
│   ├── http_stream_reader.hpp
│   ├── http_stream_reader.cpp
│   ├── format_probe.hpp
│   ├── format_probe.cpp
│   ├── mp3_decoder.hpp
│   └── mp3_decoder.cpp               ← 内部 include 第三方 miniMP3
│
├── miniMP3/
│   └── miniMP3/
│       └── minimp3.h                 ← 【保留】第三方解码库，不再承载自研播放逻辑
│
└── wlan/
    ├── tcp/                          ← 【已预留空目录，本次填写】
    │   ├── CMakeLists.txt
    │   ├── tcp_client.hpp
    │   ├── tcp_client.cpp
    │   ├── tcp_listener.hpp
    │   └── tcp_listener.cpp
    │
    └── dlna/                         ← 【已预留空目录，本次填写】
        ├── CMakeLists.txt
        ├── dlna_types/
        ├── xml/
        ├── event/
        ├── soap/
        ├── ssdp/
        ├── playback_bridge/
        └── renderer/

realize/audio_play/
├── audio_play.cpp
├── audio_play.hpp
├── CMakeLists.txt
├── pcm_pacer.hpp                     ← 原 push_pcm_with_closed_loop 的落点
├── pcm_pacer.cpp
├── dlna_playback_adapter.hpp         ← 把 play 绑定到 playback_bridge
├── dlna_playback_adapter.cpp
└── play_task.cpp                     ← 负责挂接 IIS 回调并启动 play_run_loop()
```

### 3.3 include 风格说明

由于当前工程已经把 `main/receiving end/includes` 加到了头文件搜索路径，因此 `play/` 可以直接走根目录子路径 include；而 `includes/miniMP3` 继续只给第三方解码头使用。

首版建议统一使用下面这种 include 风格：

```cpp
#include "play/play_service.hpp"
#include "play/mp3_decoder.hpp"
#include "tcp_client.hpp"
#include "tcp_listener.hpp"
#include "renderer/media_renderer.hpp"
#include "playback_bridge/playback_bridge.hpp"
#include "xml/xml_text.hpp"
#include "xml/xml_builder.hpp"
```

第三方解码库只在 `mp3_decoder.cpp` 内部写：

```cpp
#include "miniMP3/minimp3.h"
```

不要再让 `wifi_task.cpp`、`control_router.cpp`、`playback_bridge` 或其他上层直接 include 这个第三方头。

---

## 4. 模块依赖关系图

```text
┌────────────────────────────────────────────────────────────────────┐
│                       应用层 / wifi_task                           │
│  负责等待网络就绪、注册播放回调、启动 media_renderer::run()         │
└─────────────────────────────┬──────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────────┐
│               dlna/renderer/media_renderer.hpp                     │
│  编排层：select 循环、HTTP accept、SSDP poll、初始 NOTIFY 触发      │
└──────────────┬──────────────────┬────────────────┬─────────────────┘
               │                  │                │
               │ owns             │ owns           │ owns
               ▼                  ▼                ▼
     ┌────────────────┐   ┌────────────────┐   ┌──────────────────┐
     │ ssdp_service   │   │ control_router │   │ subscription_mgr │
     │ 发现面         │   │ 控制面路由     │   │ 订阅状态         │
     └────────────────┘   └───────┬────────┘   └─────────┬────────┘
                                   │                      │
                                   │ uses                 │ used by
                                   ▼                      ▼
                           ┌────────────────┐     ┌────────────────┐
                           │ xml_text       │     │ notify_sender  │
                           │ xml_builder    │     │ 事件发送       │
                           └────────────────┘     └───────┬────────┘
                                                           │
                                                           │ uses
                                                           ▼
                                                   ┌────────────────┐
                                                   │ tcp_client     │
                                                   │ 主动 TCP       │
                                                   └────────────────┘

control_router 通过 playback_bridge 发命令，不直接碰 play 内部实现。

playback_bridge -> 应用层注册的 play 适配器

play/play_service -> playback_session + http_stream_reader + mp3_decoder + pcm_pacer

mp3_decoder 内部使用外部 miniMP3 解码库，但这个细节不再向控制面暴露。

tcp_listener 只负责 listen/accept，不感知 DLNA 协议。
xml 模块只处理文本，不感知 socket。
```

---

## 5. dlna_types 共享类型模块

### 5.1 职责说明

`dlna_types` 是整个 DLNA 控制面的共享数据层。它只定义：

- 服务种类枚举
- 传输状态枚举
- 播放命令结构
- 订阅槽结构
- renderer 的运行时状态结构
- 控制路由的响应结构

它不做任何 socket 操作，也不拼 XML。

### 5.2 `includes/wlan/dlna/dlna_types/dlna_types.hpp`

```cpp
#pragma once

extern "C" {
#include "common_def.h"
}

#include <cstdint>

namespace sed_ws63 {

enum class dlna_service_kind : uint8_t {
    avtransport = 0,
    rendering_control,
    connection_manager,
    unknown,
};

enum class transport_state : uint8_t {
    stopped = 0,
    transitioning,
    playing,
    paused_playback,
    no_media_present,
};

enum class playback_command_type : uint8_t {
    set_uri = 0,
    play,
    pause,
    stop,
};

struct playback_command {
    playback_command_type type;
    char uri[512];
    char metadata[512];
};

struct subscribe_slot {
    char sid[128];
    char callback[256];
    uint32_t seq;
    uint32_t timeout_sec;
    bool active;
};

struct renderer_state {
    char current_uri[512];
    char current_metadata[512];
    transport_state state;
    uint8_t volume;
    bool mute;
    uint32_t elapsed_base_sec;
    unsigned long long playback_started_jiffies;
};

struct control_response {
    int http_status;
    char status_text[32];
    char content_type[64];
    char body[2048];
    bool send_initial_avtransport_notify;
    bool send_initial_rendering_notify;
};

renderer_state make_default_renderer_state();
control_response make_default_control_response();
const char *transport_state_text(transport_state state);

} // namespace sed_ws63
```

### 5.3 `includes/wlan/dlna/dlna_types/dlna_types.cpp`

```cpp
#include <cstdio>

#include "dlna_types.hpp"

namespace sed_ws63 {

renderer_state make_default_renderer_state()
{
    renderer_state state = {};
    state.state = transport_state::stopped;
    state.volume = 50;
    state.mute = false;
    state.elapsed_base_sec = 0;
    state.playback_started_jiffies = 0;
    return state;
}

control_response make_default_control_response()
{
    control_response resp = {};
    resp.http_status = 200;
    resp.send_initial_avtransport_notify = false;
    resp.send_initial_rendering_notify = false;
    (void)snprintf(resp.status_text, sizeof(resp.status_text), "OK");
    (void)snprintf(resp.content_type, sizeof(resp.content_type), "text/xml; charset=\"utf-8\"");
    return resp;
}

const char *transport_state_text(transport_state state)
{
    switch (state) {
        case transport_state::stopped:
            return "STOPPED";
        case transport_state::transitioning:
            return "TRANSITIONING";
        case transport_state::playing:
            return "PLAYING";
        case transport_state::paused_playback:
            return "PAUSED_PLAYBACK";
        case transport_state::no_media_present:
            return "NO_MEDIA_PRESENT";
        default:
            return "STOPPED";
    }
}

} // namespace sed_ws63
```

### 5.4 为什么要单独做这一层

这样做之后：

- `control_router` 不再直接持有一堆文件作用域静态变量
- `subscription_manager` 不需要依赖 `media_renderer`
- `playback_bridge` 不需要知道 XML 细节
- 状态从“散在 dlan.cpp 的匿名 namespace”变成了显式契约

---

## 6. tcp 模块

### 6.1 职责说明

你明确要求把 TCP 分离，这个要求是对的，而且必须拆成两类能力：

- `tcp_client`：给 NOTIFY 这种主动连接使用
- `tcp_listener`：给 HTTP 控制面监听和 accept 使用

当前 dlan.cpp 同时有：

- 主动 TCP connect 给控制点发 NOTIFY
- 被动 listen/accept 等手机发控制请求

它们虽然都叫 TCP，但职责完全不同，应该拆成两个类。

### 6.2 `includes/wlan/tcp/tcp_client.hpp`

```cpp
#pragma once

extern "C" {
#include "lwip/sockets.h"
#include "netinet/in.h"
#include "common_def.h"
}

#include <cstdint>

namespace sed_ws63 {

class tcp_client {
public:
    tcp_client() = default;
    ~tcp_client();

    // 建立到目标 host:port 的连接，并顺手配置收发超时。
    errcode_t connect_host(const char *host, uint16_t port, uint32_t timeout_ms);
    // 尽量把整个缓冲区发完；适合发送完整 HTTP 请求报文。
    int32_t send_all(const void *buf, uint16_t len);
    // 读取一小段数据；用于读取 HTTP 响应头或短响应即可。
    int32_t recv_some(void *buf, uint16_t len, uint32_t timeout_ms);
    // 主动关闭连接，允许同一个对象被重复复用。
    void close();
    // 仅判断本地 fd 是否有效，不代表对端链路一定还活着。
    bool is_open() const;

private:
    // -1 表示当前没有持有任何活动 socket。
    int32_t sfd_ = -1;
};

} // namespace sed_ws63
```

### 6.3 `includes/wlan/tcp/tcp_client.cpp`

```cpp
#include "tcp_client.hpp"

#include "wifi_tool.hpp"

using wifi_tool_t = sed_ws63::wifi_tool;

namespace sed_ws63 {

tcp_client::~tcp_client()
{
    // RAII：对象析构时兜底关闭，避免上层漏掉 close。
    close();
}

errcode_t tcp_client::connect_host(const char *host, uint16_t port, uint32_t timeout_ms)
{
    // 一个 tcp_client 在任意时刻只维护一个连接，先把旧连接清掉。
    close();

    sfd_ = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sfd_ < 0) {
        return ERRCODE_FAIL;
    }

    // 连接建立后统一把收发超时写进 socket，避免上层每次都配。
    timeval tv = {
        static_cast<long>(timeout_ms / 1000),
        static_cast<long>((timeout_ms % 1000) * 1000),
    };
    (void)lwip_setsockopt(sfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)lwip_setsockopt(sfd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // 这里只负责 TCP 连接；域名/IP 解析改走新的 wifi_tool。
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(port);
    if (wifi_tool_t::get_ipv4_addr(host, &addr.sin_addr) != ERRCODE_SUCC) {
        close();
        return ERRCODE_FAIL;
    }

    // 任何一步失败都回滚到“未连接”状态，避免留下半初始化 fd。
    if (lwip_connect(sfd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close();
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCC;
}

int32_t tcp_client::send_all(const void *buf, uint16_t len)
{
    if (sfd_ < 0 || buf == nullptr || len == 0) {
        return -1;
    }

    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(buf);
    int32_t sent_total = 0;
    // lwIP 的 send 可能一次只发送部分数据，因此这里必须循环补齐。
    while (sent_total < static_cast<int32_t>(len)) {
        int32_t ret = lwip_send(sfd_, ptr + sent_total, len - sent_total, 0);
        if (ret <= 0) {
            return -1;
        }
        sent_total += ret;
    }

    return sent_total;
}

int32_t tcp_client::recv_some(void *buf, uint16_t len, uint32_t timeout_ms)
{
    if (sfd_ < 0 || buf == nullptr || len == 0) {
        return -1;
    }

    // 接收超时允许按场景单独调整，例如 NOTIFY 的回包通常只等很短时间。
    timeval tv = {
        static_cast<long>(timeout_ms / 1000),
        static_cast<long>((timeout_ms % 1000) * 1000),
    };
    (void)lwip_setsockopt(sfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return lwip_recv(sfd_, buf, len, 0);
}

void tcp_client::close()
{
    // close 设计成幂等操作，方便上层和析构重复调用。
    if (sfd_ >= 0) {
        lwip_close(sfd_);
        sfd_ = -1;
    }
}

bool tcp_client::is_open() const
{
    return sfd_ >= 0;
}

} // namespace sed_ws63
```

### 6.4 `includes/wlan/tcp/tcp_listener.hpp`

```cpp
#pragma once

extern "C" {
#include "lwip/sockets.h"
#include "netinet/in.h"
#include "common_def.h"
}

#include <cstdint>

namespace sed_ws63 {

class tcp_listener {
public:
    tcp_listener() = default;
    ~tcp_listener();

    // 建立监听 socket；默认绑定所有地址并允许少量排队连接。
    errcode_t listen_on(uint16_t port, const char *bind_ip = nullptr, uint8_t backlog = 2);
    // 接受一个新连接；如果需要，顺便把对端地址抄给调用者。
    int32_t accept_one(sockaddr_in *peer_addr = nullptr);
    // 停止监听并释放底层 socket。
    void close();
    // 仅表示监听 fd 是否已经创建成功。
    bool is_listening() const;
    // 暴露底层 fd，给 select/poll 这类编排层使用。
    int32_t fd() const;

private:
    // -1 表示当前未进入监听状态。
    int32_t sfd_ = -1;
};

} // namespace sed_ws63
```

### 6.5 `includes/wlan/tcp/tcp_listener.cpp`

```cpp
#include "tcp_listener.hpp"

namespace sed_ws63 {

tcp_listener::~tcp_listener()
{
    // 析构时保证释放监听口，避免任务退出后端口残留。
    close();
}

errcode_t tcp_listener::listen_on(uint16_t port, const char *bind_ip, uint8_t backlog)
{
    // 如果之前已经 listen 过，先把旧监听 socket 干净关闭。
    close();

    sfd_ = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sfd_ < 0) {
        return ERRCODE_FAIL;
    }

    // 允许短时间内重复绑定，减少设备重启/任务重拉起时的端口占用问题。
    int32_t reuse = 1;
    (void)lwip_setsockopt(sfd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // bind_ip 为空时监听所有本地地址；有需要也可以只绑定指定 IP。
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(port);
    addr.sin_addr.s_addr = (bind_ip == nullptr) ? INADDR_ANY : inet_addr(bind_ip);

    // bind 和 listen 任一步失败都统一 close，避免悬空 fd。
    if (lwip_bind(sfd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close();
        return ERRCODE_FAIL;
    }

    if (lwip_listen(sfd_, backlog) != 0) {
        close();
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCC;
}

int32_t tcp_listener::accept_one(sockaddr_in *peer_addr)
{
    if (sfd_ < 0) {
        return -1;
    }

    // accept 返回的是新的 client fd；监听 fd 本身继续保留。
    sockaddr_in peer = {};
    socklen_t peer_len = sizeof(peer);
    int32_t client = lwip_accept(sfd_, reinterpret_cast<sockaddr *>(&peer), &peer_len);
    if (client >= 0 && peer_addr != nullptr) {
        *peer_addr = peer;
    }
    return client;
}

void tcp_listener::close()
{
    // 和 tcp_client 一样，close 保持幂等，方便编排层兜底调用。
    if (sfd_ >= 0) {
        lwip_close(sfd_);
        sfd_ = -1;
    }
}

bool tcp_listener::is_listening() const
{
    return sfd_ >= 0;
}

int32_t tcp_listener::fd() const
{
    return sfd_;
}

} // namespace sed_ws63
```

### 6.6 这个模块拆出来后得到什么

拆出来后：

- `notify_sender` 不再直接写 socket connect 细节
- `media_renderer` 不再自己关心 bind/listen/accept 细节
- 后续如果你想给 NOTIFY 增加更严格的错误码或超时策略，只改 tcp 层即可

---

## 7. xml 模块

### 7.1 为什么 XML 必须独立

你已经明确提出要拆 XML，这个判断是正确的。

当前实现里混在一起的其实是三件不同的事：

- XML 文本转义与反转义
- XML 标签值提取
- 整个 SOAP/设备描述/事件通知 XML 的构造

它们都属于“文本层”，不应该跟 socket、select、playback 回调混在一起。

### 7.2 推荐拆分方式

XML 模块建议拆成两个文件：

- `xml_text`：只处理字符串级别的转义、反转义、标签提取
- `xml_builder`：只处理完整 XML 文档拼装

### 7.3 `includes/wlan/dlna/xml/xml_text.hpp`

```cpp
#pragma once

extern "C" {
#include "common_def.h"
}

#include <cstddef>

namespace sed_ws63 {

errcode_t xml_escape_basic(char *dst, size_t dst_size, const char *src);
errcode_t xml_decode_basic(char *text);
char *xml_extract_tag_value(const char *buffer, const char *tag, char *out, size_t out_size);

} // namespace sed_ws63
```

### 7.4 `includes/wlan/dlna/xml/xml_text.cpp`

```cpp
#include "xml_text.hpp"

#include "wifi_tool.hpp"

using wifi_tool_t = sed_ws63::wifi_tool;

namespace sed_ws63 {

errcode_t xml_escape_basic(char *dst, size_t dst_size, const char *src)
{
    if (dst == nullptr || dst_size == 0 || src == nullptr) {
        return ERRCODE_FAIL;
    }

    size_t di = 0;
    for (size_t si = 0; src[si] != '\0'; ++si) {
        const char *replace = nullptr;
        switch (src[si]) {
            case '&': replace = "&amp;"; break;
            case '<': replace = "&lt;"; break;
            case '>': replace = "&gt;"; break;
            case '"': replace = "&quot;"; break;
            case '\'': replace = "&apos;"; break;
            default: break;
        }

        if (replace != nullptr) {
            for (size_t ri = 0; replace[ri] != '\0'; ++ri) {
                if (di + 1 >= dst_size) {
                    dst[di] = '\0';
                    return ERRCODE_FAIL;
                }
                dst[di++] = replace[ri];
            }
            continue;
        }

        if (di + 1 >= dst_size) {
            dst[di] = '\0';
            return ERRCODE_FAIL;
        }
        dst[di++] = src[si];
    }

    dst[di] = '\0';
    return ERRCODE_SUCC;
}

errcode_t xml_decode_basic(char *text)
{
    if (text == nullptr) {
        return ERRCODE_FAIL;
    }

    // wifi_tool 在“没有可解码实体”时会返回 0x03；对 XML 层来说这不算失败。
    errcode_t ret = wifi_tool_t::decode_xml_basic(text);
    return (ret == ERRCODE_SUCC || ret == 0x03) ? ERRCODE_SUCC : ERRCODE_FAIL;
}

char *xml_extract_tag_value(const char *buffer, const char *tag, char *out, size_t out_size)
{
    if (buffer == nullptr || tag == nullptr || out == nullptr || out_size == 0) {
        return nullptr;
    }

    wifi_tool_t::span_text span = wifi_tool_t::find_html_tag_value(buffer, tag);
    if (span.ptr == nullptr) {
        return nullptr;
    }
    return (wifi_tool_t::copy_str(out, out_size, span.ptr, span.len) == ERRCODE_SUCC) ? out : nullptr;
}

} // namespace sed_ws63
```

### 7.5 `includes/wlan/dlna/xml/xml_builder.hpp`

```cpp
#pragma once

#include "dlna_types/dlna_types.hpp"

#include <cstddef>
#include <cstdint>

namespace sed_ws63 {

int build_device_description_xml(char *out, size_t out_size, const char *local_ip, uint16_t http_port, const char *udn);
int build_avtransport_service_xml(char *out, size_t out_size);
int build_rendering_control_service_xml(char *out, size_t out_size);
int build_connection_manager_service_xml(char *out, size_t out_size);
int build_soap_action_response(char *out, size_t out_size, const char *service_ns, const char *action_name,
                               const char *inner_xml);
int build_soap_fault_response(char *out, size_t out_size, int upnp_error_code, const char *description);
int build_avtransport_event_xml(char *out, size_t out_size, const renderer_state &state);
int build_rendering_event_xml(char *out, size_t out_size, const renderer_state &state);

} // namespace sed_ws63
```

### 7.6 `includes/wlan/dlna/xml/xml_builder.cpp`

```cpp
#include "xml_builder.hpp"
#include "xml_text.hpp"

#include <cstdio>

namespace sed_ws63 {

int build_device_description_xml(char *out, size_t out_size, const char *local_ip, uint16_t http_port, const char *udn)
{
    if (out == nullptr || out_size == 0 || local_ip == nullptr || udn == nullptr) {
        return -1;
    }

    return snprintf(out, out_size,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" "
                    "xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">\r\n"
                    "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
                    "  <device>\r\n"
                    "    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>\r\n"
                    "    <friendlyName>ws63_sound</friendlyName>\r\n"
                    "    <manufacturer>sEvil_Dragon</manufacturer>\r\n"
                    "    <modelDescription>Audio Device Based on HiSilicon WS63</modelDescription>\r\n"
                    "    <modelName>WS63-DMR</modelName>\r\n"
                    "    <modelNumber>1.0</modelNumber>\r\n"
                    "    <UDN>%s</UDN>\r\n"
                    "    <presentationURL>http://%s:%u/</presentationURL>\r\n"
                    "    <dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>\r\n"
                    "    <serviceList>\r\n"
                    "      <service>\r\n"
                    "        <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>\r\n"
                    "        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>\r\n"
                    "        <SCPDURL>/AVTransport.xml</SCPDURL>\r\n"
                    "        <controlURL>/AVTransport/control</controlURL>\r\n"
                    "        <eventSubURL>/AVTransport/event</eventSubURL>\r\n"
                    "      </service>\r\n"
                    "      <service>\r\n"
                    "        <serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>\r\n"
                    "        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>\r\n"
                    "        <SCPDURL>/RenderingControl.xml</SCPDURL>\r\n"
                    "        <controlURL>/RenderingControl/control</controlURL>\r\n"
                    "        <eventSubURL>/RenderingControl/event</eventSubURL>\r\n"
                    "      </service>\r\n"
                    "      <service>\r\n"
                    "        <serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>\r\n"
                    "        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>\r\n"
                    "        <SCPDURL>/ConnectionManager.xml</SCPDURL>\r\n"
                    "        <controlURL>/ConnectionManager/control</controlURL>\r\n"
                    "        <eventSubURL>/ConnectionManager/event</eventSubURL>\r\n"
                    "      </service>\r\n"
                    "    </serviceList>\r\n"
                    "  </device>\r\n"
                    "</root>\r\n",
                    udn, local_ip, http_port);
}

int build_avtransport_service_xml(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return -1;
    }

    return snprintf(out, out_size,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
                    "  <actionList>\r\n"
                    "    <action><name>SetAVTransportURI</name></action>\r\n"
                    "    <action><name>Play</name></action>\r\n"
                    "    <action><name>Pause</name></action>\r\n"
                    "    <action><name>Stop</name></action>\r\n"
                    "    <action><name>GetTransportInfo</name></action>\r\n"
                    "    <action><name>GetPositionInfo</name></action>\r\n"
                    "  </actionList>\r\n"
                    "</scpd>");
}

int build_rendering_control_service_xml(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return -1;
    }

    return snprintf(out, out_size,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
                    "  <actionList>\r\n"
                    "    <action><name>SetVolume</name></action>\r\n"
                    "    <action><name>GetVolume</name></action>\r\n"
                    "    <action><name>SetMute</name></action>\r\n"
                    "    <action><name>GetMute</name></action>\r\n"
                    "  </actionList>\r\n"
                    "</scpd>");
}

int build_connection_manager_service_xml(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return -1;
    }

    return snprintf(out, out_size,
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
                    "  <actionList>\r\n"
                    "    <action><name>GetProtocolInfo</name></action>\r\n"
                    "    <action><name>GetCurrentConnectionIDs</name></action>\r\n"
                    "    <action><name>GetCurrentConnectionInfo</name></action>\r\n"
                    "  </actionList>\r\n"
                    "</scpd>");
}

int build_soap_action_response(char *out, size_t out_size, const char *service_ns, const char *action_name,
                               const char *inner_xml)
{
    if (out == nullptr || out_size == 0 || service_ns == nullptr || action_name == nullptr) {
        return -1;
    }

    if (inner_xml == nullptr) {
        inner_xml = "";
    }

    return snprintf(out, out_size,
                    "<?xml version=\"1.0\"?>"
                    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                    "<s:Body>"
                    "<u:%sResponse xmlns:u=\"%s\">%s</u:%sResponse>"
                    "</s:Body>"
                    "</s:Envelope>",
                    action_name, service_ns, inner_xml, action_name);
}

int build_soap_fault_response(char *out, size_t out_size, int upnp_error_code, const char *description)
{
    if (out == nullptr || out_size == 0) {
        return -1;
    }

    if (description == nullptr) {
        description = "Invalid Args";
    }

    return snprintf(out, out_size,
                    "<?xml version=\"1.0\"?>"
                    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                    "<s:Body><s:Fault><faultcode>s:Client</faultcode>"
                    "<faultstring>UPnPError</faultstring><detail>"
                    "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
                    "<errorCode>%d</errorCode><errorDescription>%s</errorDescription>"
                    "</UPnPError></detail></s:Fault></s:Body></s:Envelope>",
                    upnp_error_code, description);
}

int build_avtransport_event_xml(char *out, size_t out_size, const renderer_state &state)
{
    if (out == nullptr || out_size == 0) {
        return -1;
    }

    return snprintf(out, out_size,
                    "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
                    "<e:property><LastChange>"
                    "&lt;Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\"&gt;"
                    "&lt;InstanceID val=\"0\"&gt;"
                    "&lt;TransportState val=\"%s\"/&gt;"
                    "&lt;/InstanceID&gt;"
                    "&lt;/Event&gt;"
                    "</LastChange></e:property></e:propertyset>",
                    transport_state_text(state.state));
}

int build_rendering_event_xml(char *out, size_t out_size, const renderer_state &state)
{
    if (out == nullptr || out_size == 0) {
        return -1;
    }

    return snprintf(out, out_size,
                    "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
                    "<e:property><LastChange>"
                    "&lt;Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\"&gt;"
                    "&lt;InstanceID val=\"0\"&gt;"
                    "&lt;Volume channel=\"Master\" val=\"%u\"/&gt;"
                    "&lt;Mute channel=\"Master\" val=\"%u\"/&gt;"
                    "&lt;/InstanceID&gt;"
                    "&lt;/Event&gt;"
                    "</LastChange></e:property></e:propertyset>",
                    static_cast<unsigned int>(state.volume), state.mute ? 1U : 0U);
}

} // namespace sed_ws63
```

这三段 SCPD XML 的内容直接对齐当前旧版 `dlan.cpp` 的静态响应，第一阶段先保持动作集合不变；后续如果你补更多 DLNA 动作，只需要继续扩这个 builder，而不是把大字符串重新塞回控制路由。

### 7.7 关于 `wifi_tool` 的边界建议

第一阶段建议这样处理：

- URL 解析、IPv4 解析、大小写无关比较等基础能力统一走 `wifi_tool`
- XML 模块对 `wifi_tool::decode_xml_basic()` 和 `wifi_tool::find_html_tag_value()` 做一层窄封装
- HTTP 头读取优先直接复用 `wifi_tool::find_http_header_value()` 和 `wifi_tool::trim_and_find_http_header_value()`；模块内部如果必须落到固定缓冲区，只保留 `span_text -> copy_str()` 这类窄转换，不再重复实现整段按行扫描逻辑

也就是说，不要再让 `control_router`、`ssdp_service` 或 `notify_sender` 各自维护一份冒号分隔和 CRLF 扫描代码，而是统一通过 `wifi_tool` 已有的头字段提取能力进入。

这样你的边界就先稳定了，而且头字段匹配语义只保留一份，后面再决定是否继续把更多 HTTP 细节下沉到专门模块。

---

## 8. event 模块

### 8.1 职责说明

订阅管理和 NOTIFY 发送是两件事：

- `subscription_manager` 只保存 SID、CALLBACK、SEQ、是否活跃
- `notify_sender` 只根据一个订阅槽把 XML 推出去

不要再把这些状态塞回 `dlan.cpp` 的匿名 namespace。

### 8.2 `includes/wlan/dlna/event/subscription_manager.hpp`

```cpp
#pragma once

#include "dlna_types/dlna_types.hpp"

namespace sed_ws63 {

class subscription_manager {
public:
    subscription_manager();

    void clear(dlna_service_kind kind);
    void upsert(dlna_service_kind kind, const char *sid, const char *callback, uint32_t timeout_sec);
    uint32_t next_seq(dlna_service_kind kind);

    subscribe_slot *get(dlna_service_kind kind);
    const subscribe_slot *get(dlna_service_kind kind) const;

private:
    subscribe_slot avtransport_;
    subscribe_slot rendering_control_;
    subscribe_slot connection_manager_;
};

} // namespace sed_ws63
```

### 8.3 `includes/wlan/dlna/event/subscription_manager.cpp`

```cpp
#include "subscription_manager.hpp"
#include "wifi_tool.hpp"

using wifi_tool_t = sed_ws63::wifi_tool;

namespace sed_ws63 {
namespace {

subscribe_slot make_empty_slot()
{
    subscribe_slot slot = {};
    slot.timeout_sec = 1800;
    slot.active = false;
    return slot;
}

} // namespace

subscription_manager::subscription_manager()
    : avtransport_(make_empty_slot()),
      rendering_control_(make_empty_slot()),
      connection_manager_(make_empty_slot())
{
}

subscribe_slot *subscription_manager::get(dlna_service_kind kind)
{
    switch (kind) {
        case dlna_service_kind::avtransport:
            return &avtransport_;
        case dlna_service_kind::rendering_control:
            return &rendering_control_;
        case dlna_service_kind::connection_manager:
            return &connection_manager_;
        default:
            return nullptr;
    }
}

const subscribe_slot *subscription_manager::get(dlna_service_kind kind) const
{
    return const_cast<subscription_manager *>(this)->get(kind);
}

void subscription_manager::clear(dlna_service_kind kind)
{
    subscribe_slot *slot = get(kind);
    if (slot != nullptr) {
        *slot = make_empty_slot();
    }
}

void subscription_manager::upsert(dlna_service_kind kind, const char *sid, const char *callback, uint32_t timeout_sec)
{
    subscribe_slot *slot = get(kind);
    if (slot == nullptr) {
        return;
    }

    // 第一阶段按“一个服务一个订阅槽”处理，新订阅直接覆盖旧状态。
    *slot = make_empty_slot();
    slot->timeout_sec = (timeout_sec == 0) ? 1800 : timeout_sec;
    slot->active = true;
    (void)wifi_tool_t::copy_str(slot->sid, sizeof(slot->sid), sid);
    (void)wifi_tool_t::copy_str(slot->callback, sizeof(slot->callback), callback);
}

uint32_t subscription_manager::next_seq(dlna_service_kind kind)
{
    subscribe_slot *slot = get(kind);
    if (slot == nullptr) {
        return 0;
    }
    // 先返回当前 SEQ，再自增，保证首次 NOTIFY 从 0 开始。
    return slot->seq++;
}

} // namespace sed_ws63
```

### 8.4 `includes/wlan/dlna/event/notify_sender.hpp`

```cpp
#pragma once

#include "dlna_types/dlna_types.hpp"

namespace sed_ws63 {

class notify_sender {
public:
    errcode_t send_event(const subscribe_slot &slot, uint32_t seq, const char *body);
};

} // namespace sed_ws63
```

### 8.5 `includes/wlan/dlna/event/notify_sender.cpp`

```cpp
#include "notify_sender.hpp"
#include "tcp_client.hpp"

#include "wifi_tool.hpp"

using wifi_tool_t = sed_ws63::wifi_tool;

namespace sed_ws63 {

errcode_t notify_sender::send_event(const subscribe_slot &slot, uint32_t seq, const char *body)
{
    if (!slot.active || slot.callback[0] == '\0' || slot.sid[0] == '\0' || body == nullptr) {
        return ERRCODE_FAIL;
    }

    // CALLBACK 保存的是控制点给出的回调 URL，这里统一解析成 host/port/path。
    wifi_tool_t::parse_url url = {};
    // 第一阶段仍只接受 http:// 回调，不把 wifi_tool 的 https 解析能力透传到无 TLS 链路。
    if (wifi_tool_t::get_url(slot.callback, url) != ERRCODE_SUCC ||
        !wifi_tool_t::strcmp_ignore_case(url.scheme.data(), "http")) {
        return ERRCODE_FAIL;
    }

    // NOTIFY 是主动连接，不复用 renderer 的监听 socket。
    tcp_client client;
    if (client.connect_host(url.host.data(), url.port, 2000) != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }

    // 直接组完整 HTTP NOTIFY 报文，避免上层再关心 TCP 细节。
    char request[1536] = {0};
    int body_len = static_cast<int>(strlen(body));
    int req_len = snprintf(request, sizeof(request),
                           "NOTIFY %s HTTP/1.1\r\n"
                           "HOST: %s:%u\r\n"
                           "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                           "NT: upnp:event\r\n"
                           "NTS: upnp:propchange\r\n"
                           "SID: %s\r\n"
                           "SEQ: %u\r\n"
                           "CONTENT-LENGTH: %d\r\n"
                           "Connection: close\r\n\r\n"
                           "%s",
                           url.path.data(), url.host.data(), url.port, slot.sid, seq, body_len, body);
    if (req_len <= 0 || req_len >= static_cast<int>(sizeof(request))) {
        return ERRCODE_FAIL;
    }

    if (client.send_all(request, static_cast<uint16_t>(req_len)) <= 0) {
        return ERRCODE_FAIL;
    }

    // 这里只做一次简短读取，把对端响应读掉即可，不在这里做复杂状态机。
    char response[256] = {0};
    (void)client.recv_some(response, sizeof(response) - 1, 1000);
    return ERRCODE_SUCC;
}

} // namespace sed_ws63
```

### 8.6 为什么要这样拆

这样拆完以后：

- SUBSCRIBE 不再直接修改全局静态数组
- NOTIFY 的 TCP 失败不会污染 renderer 主类
- 以后如果要扩展第三个订阅服务，不需要再复制一组 `g_xxx_callback / g_xxx_sid / g_xxx_seq`

---

## 9. soap 控制路由模块

### 9.1 职责说明

这个模块负责：

- 解析 HTTP 请求首行与关键头字段
- 区分 GET / SUBSCRIBE / UNSUBSCRIBE / POST
- 对 POST 控制请求按 SOAPAction 分发
- 生成响应 body

这个模块不负责：

- listen / accept
- SSDP
- 真正的播放实现
- NOTIFY 网络发送

### 9.2 `includes/wlan/dlna/soap/control_router.hpp`

```cpp
#pragma once

#include "dlna_types/dlna_types.hpp"

namespace sed_ws63 {

class subscription_manager;
class playback_bridge;

class control_router {
public:
    errcode_t handle_request(const char *request,
                             const char *local_ip,
                             uint16_t http_port,
                             const char *udn,
                             renderer_state *state,
                             subscription_manager *subs,
                             playback_bridge *bridge,
                             control_response *out);

private:
    errcode_t handle_get_(const char *request,
                          const char *local_ip,
                          uint16_t http_port,
                          const char *udn,
                          renderer_state *state,
                          control_response *out);
    errcode_t handle_subscribe_(const char *request, subscription_manager *subs, control_response *out);
    errcode_t handle_unsubscribe_(const char *request, subscription_manager *subs, control_response *out);
    errcode_t handle_post_(const char *request,
                           renderer_state *state,
                           subscription_manager *subs,
                           playback_bridge *bridge,
                           control_response *out);
};

} // namespace sed_ws63
```

### 9.3 控制路由的关键实现思路

控制路由建议维持下面这个顺序：

1. 读请求首行，判断方法
2. GET 直接返回描述文档或 service XML
3. SUBSCRIBE / UNSUBSCRIBE 只更新订阅状态，不碰播放命令
4. POST 再进入 SOAPAction 分发

下面给出一个可直接落地的 `handle_request()` 骨架。这里不要再在 `control_router.cpp` 里手写一份 `find_http_header_value_()`；当前 `wifi_tool` 已经提供了对应能力，路由层只需要消费返回的 `span_text`：

```cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "control_router.hpp"
#include "event/subscription_manager.hpp"
#include "playback_bridge/playback_bridge.hpp"
#include "xml/xml_builder.hpp"
#include "xml/xml_text.hpp"

#include "wifi_tool.hpp"

namespace sed_ws63 {
namespace {

using wifi_tool_t = sed_ws63::wifi_tool;

const char *find_http_body(const char *request)
{
    if (request == nullptr) {
        return nullptr;
    }
    // 教程示例默认请求头和 body 之间使用标准 CRLFCRLF 分隔。
    const char *body = strstr(request, "\r\n\r\n");
    return (body == nullptr) ? nullptr : (body + 4);
}

void strip_quotes(char *text)
{
    if (text == nullptr) {
        return;
    }
    // SOAPACTION 头通常带双引号，这里做一次原地清洗。
    (void)wifi_tool_t::trim(text);
    (void)wifi_tool_t::strip(text, '"', '"');
}

bool looks_like_mp3_candidate(const char *uri, const char *metadata)
{
    // 第一阶段先沿用当前项目的能力边界，只接收明显的 MP3 资源。
    if (metadata != nullptr && metadata[0] != '\0') {
        if (wifi_tool_t::is_strstr_ignore_case(metadata, "audio/mpeg") ||
            wifi_tool_t::is_strstr_ignore_case(metadata, "audio/mp3") ||
            wifi_tool_t::is_strstr_ignore_case(metadata, "audio/x-mpeg") ||
            wifi_tool_t::is_strstr_ignore_case(metadata, "mp3")) {
            return true;
        }
        return false;
    }

    if (uri == nullptr || uri[0] == '\0') {
        return false;
    }
    if (wifi_tool_t::is_strstr_ignore_case(uri, ".mp3") ||
        wifi_tool_t::is_strstr_ignore_case(uri, "format=mp3") ||
        wifi_tool_t::is_strstr_ignore_case(uri, "mime=audio/mpeg")) {
        return true;
    }

    return true;
}

} // namespace

errcode_t control_router::handle_request(const char *request,
                                         const char *local_ip,
                                         uint16_t http_port,
                                         const char *udn,
                                         renderer_state *state,
                                         subscription_manager *subs,
                                         playback_bridge *bridge,
                                         control_response *out)
{
    if (request == nullptr || state == nullptr || subs == nullptr || bridge == nullptr || out == nullptr) {
        return ERRCODE_FAIL;
    }

    // 每次请求都先拿一份默认响应，再按分支覆盖。
    *out = make_default_control_response();

    // 这里仅做方法级分发，具体业务逻辑下沉到各 handle_xxx_。
    if (strncmp(request, "GET ", 4) == 0 || strncmp(request, "HEAD ", 5) == 0) {
        return handle_get_(request, local_ip, http_port, udn, state, out);
    }
    if (strncmp(request, "SUBSCRIBE ", 10) == 0) {
        return handle_subscribe_(request, subs, out);
    }
    if (strncmp(request, "UNSUBSCRIBE ", 12) == 0) {
        return handle_unsubscribe_(request, subs, out);
    }
    if (strncmp(request, "POST ", 5) == 0) {
        return handle_post_(request, state, subs, bridge, out);
    }

    out->http_status = 400;
    (void)snprintf(out->status_text, sizeof(out->status_text), "Bad Request");
    (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
    (void)snprintf(out->body, sizeof(out->body), "unsupported request");
    return ERRCODE_FAIL;
}

} // namespace sed_ws63
```

### 9.4 AVTransport 路由示例

最关键的动作是 `SetAVTransportURI`、`Play`、`Pause`、`Stop`。下面给出第一阶段建议实现：

```cpp
errcode_t control_router::handle_post_(const char *request,
                                       renderer_state *state,
                                       subscription_manager *subs,
                                       playback_bridge *bridge,
                                       control_response *out)
{
    unused(subs);

    // POST 控制请求的核心入口是 SOAPACTION，没有它就无法分发动作。
    char soap_action[128] = {0};
    wifi_tool_t::span_text soap_action_span = wifi_tool_t::find_http_header_value(request, "SOAPACTION");
    if (soap_action_span.ptr == nullptr ||
        wifi_tool_t::copy_str(soap_action, sizeof(soap_action), soap_action_span.ptr, soap_action_span.len) != ERRCODE_SUCC) {
        out->http_status = 500;
        (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
        (void)build_soap_fault_response(out->body, sizeof(out->body), 401, "Invalid Action");
        return ERRCODE_FAIL;
    }

    strip_quotes(soap_action);
    // SOAP body 后面还要解析 CurrentURI 等字段，所以先定位 body 指针。
    const char *body = find_http_body(request);
    if (body == nullptr) {
        (void)build_soap_fault_response(out->body, sizeof(out->body), 402, "Invalid Args");
        out->http_status = 500;
        (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
        return ERRCODE_FAIL;
    }

    if (wifi_tool_t::is_strstr_ignore_case(soap_action, "SetAVTransportURI")) {
        char uri[512] = {0};
        char metadata[512] = {0};
        // SetURI 只负责保存和校验资源，不在这里抢先做网络探测。
        if (xml_extract_tag_value(body, "CurrentURI", uri, sizeof(uri)) == nullptr) {
            (void)build_soap_fault_response(out->body, sizeof(out->body), 402, "CurrentURI Missing");
            out->http_status = 500;
            (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
            return ERRCODE_FAIL;
        }
        (void)xml_extract_tag_value(body, "CurrentURIMetaData", metadata, sizeof(metadata));
        // 控制点经常会把 URI 和 metadata 做 XML 实体转义，这里统一恢复。
        (void)xml_decode_basic(uri);
        (void)xml_decode_basic(metadata);

        if (!looks_like_mp3_candidate(uri, metadata)) {
            (void)build_soap_fault_response(out->body, sizeof(out->body), 714, "Illegal MIME-Type");
            out->http_status = 500;
            (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
            return ERRCODE_FAIL;
        }

        if (bridge->set_uri(uri, metadata) != ERRCODE_SUCC) {
            (void)build_soap_fault_response(out->body, sizeof(out->body), 701, "Transition not available");
            out->http_status = 500;
            (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
            return ERRCODE_FAIL;
        }

        // bridge 成功后，再把 renderer 自己的镜像状态更新到最新。
        (void)wifi_tool_t::copy_str(state->current_uri, sizeof(state->current_uri), uri);
        (void)wifi_tool_t::copy_str(state->current_metadata, sizeof(state->current_metadata), metadata);
        state->state = transport_state::stopped;
        (void)build_soap_action_response(out->body, sizeof(out->body),
                                         "urn:schemas-upnp-org:service:AVTransport:1",
                                         "SetAVTransportURI", "");
        return ERRCODE_SUCC;
    }

    if (wifi_tool_t::is_strstr_ignore_case(soap_action, "Play")) {
        if (bridge->play() != ERRCODE_SUCC) {
            (void)build_soap_fault_response(out->body, sizeof(out->body), 701, "Transition not available");
            out->http_status = 500;
            (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
            return ERRCODE_FAIL;
        }

        state->state = transport_state::playing;
        (void)build_soap_action_response(out->body, sizeof(out->body),
                                         "urn:schemas-upnp-org:service:AVTransport:1",
                                         "Play", "");
        return ERRCODE_SUCC;
    }

    if (wifi_tool_t::is_strstr_ignore_case(soap_action, "Pause")) {
        if (bridge->pause() != ERRCODE_SUCC) {
            (void)build_soap_fault_response(out->body, sizeof(out->body), 701, "Transition not available");
            out->http_status = 500;
            (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
            return ERRCODE_FAIL;
        }

        state->state = transport_state::paused_playback;
        (void)build_soap_action_response(out->body, sizeof(out->body),
                                         "urn:schemas-upnp-org:service:AVTransport:1",
                                         "Pause", "");
        return ERRCODE_SUCC;
    }

    if (wifi_tool_t::is_strstr_ignore_case(soap_action, "Stop")) {
        if (bridge->stop() != ERRCODE_SUCC) {
            (void)build_soap_fault_response(out->body, sizeof(out->body), 701, "Transition not available");
            out->http_status = 500;
            (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
            return ERRCODE_FAIL;
        }

        state->state = transport_state::stopped;
        (void)build_soap_action_response(out->body, sizeof(out->body),
                                         "urn:schemas-upnp-org:service:AVTransport:1",
                                         "Stop", "");
        return ERRCODE_SUCC;
    }

    if (wifi_tool_t::is_strstr_ignore_case(soap_action, "GetTransportInfo")) {
        char inner[256] = {0};
        (void)snprintf(inner, sizeof(inner),
                       "<CurrentTransportState>%s</CurrentTransportState>"
                       "<CurrentTransportStatus>OK</CurrentTransportStatus>"
                       "<CurrentSpeed>1</CurrentSpeed>",
                       transport_state_text(state->state));
        (void)build_soap_action_response(out->body, sizeof(out->body),
                                         "urn:schemas-upnp-org:service:AVTransport:1",
                                         "GetTransportInfo", inner);
        return ERRCODE_SUCC;
    }

    (void)build_soap_fault_response(out->body, sizeof(out->body), 401, "Invalid Action");
    out->http_status = 500;
    (void)snprintf(out->status_text, sizeof(out->status_text), "Internal Server Error");
    return ERRCODE_FAIL;
}
```

### 9.5 GET/HEAD、SUBSCRIBE、UNSUBSCRIBE 处理建议

`GET /description.xml` 和三个 service description XML 也应该由 `control_router` 统一返回，但它需要编排层传入 `local_ip/http_port/udn`。建议直接这样写：

```cpp
errcode_t control_router::handle_get_(const char *request,
                                      const char *local_ip,
                                      uint16_t http_port,
                                      const char *udn,
                                      renderer_state *state,
                                      control_response *out)
{
    unused(state);

    // 根路径和 description.xml 统一落到设备描述文档，便于手机端直接探活。
    if (strstr(request, "GET /description.xml") != nullptr || strstr(request, "HEAD /description.xml") != nullptr ||
        strstr(request, "GET / HTTP/1.1") != nullptr || strstr(request, "GET / HTTP/1.0") != nullptr ||
        strstr(request, "HEAD / HTTP/1.1") != nullptr || strstr(request, "HEAD / HTTP/1.0") != nullptr) {
        (void)build_device_description_xml(out->body, sizeof(out->body), local_ip, http_port, udn);
        return ERRCODE_SUCC;
    }

    if (strstr(request, "GET /AVTransport.xml") != nullptr || strstr(request, "HEAD /AVTransport.xml") != nullptr) {
        (void)build_avtransport_service_xml(out->body, sizeof(out->body));
        return ERRCODE_SUCC;
    }

    if (strstr(request, "GET /RenderingControl.xml") != nullptr ||
        strstr(request, "HEAD /RenderingControl.xml") != nullptr) {
        (void)build_rendering_control_service_xml(out->body, sizeof(out->body));
        return ERRCODE_SUCC;
    }

    if (strstr(request, "GET /ConnectionManager.xml") != nullptr ||
        strstr(request, "HEAD /ConnectionManager.xml") != nullptr) {
        (void)build_connection_manager_service_xml(out->body, sizeof(out->body));
        return ERRCODE_SUCC;
    }

    out->http_status = 404;
    (void)snprintf(out->status_text, sizeof(out->status_text), "Not Found");
    (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
    (void)snprintf(out->body, sizeof(out->body), "not found");
    return ERRCODE_FAIL;
}

errcode_t control_router::handle_unsubscribe_(const char *request, subscription_manager *subs, control_response *out)
{
    dlna_service_kind kind = dlna_service_kind::unknown;
    if (strstr(request, "UNSUBSCRIBE /AVTransport/event") != nullptr) {
        kind = dlna_service_kind::avtransport;
    } else if (strstr(request, "UNSUBSCRIBE /RenderingControl/event") != nullptr) {
        kind = dlna_service_kind::rendering_control;
    } else if (strstr(request, "UNSUBSCRIBE /ConnectionManager/event") != nullptr) {
        kind = dlna_service_kind::connection_manager;
    }

    if (kind == dlna_service_kind::unknown) {
        out->http_status = 400;
        (void)snprintf(out->status_text, sizeof(out->status_text), "Bad Request");
        (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
        (void)snprintf(out->body, sizeof(out->body), "invalid unsubscribe");
        return ERRCODE_FAIL;
    }

    subs->clear(kind);
    out->http_status = 200;
    (void)snprintf(out->status_text, sizeof(out->status_text), "OK");
    (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
    out->body[0] = '\0';
    return ERRCODE_SUCC;
}
```

这部分不属于 SOAPAction，但仍属于控制路由模块，建议这样处理：

```cpp
errcode_t control_router::handle_subscribe_(const char *request, subscription_manager *subs, control_response *out)
{
    char sid[128] = {0};
    char callback[256] = {0};
    char timeout_text[64] = {0};
    uint32_t timeout_sec = 1800;

    const wifi_tool_t::span_text sid_span = wifi_tool_t::find_http_header_value(request, "SID");
    const wifi_tool_t::span_text callback_span = wifi_tool_t::find_http_header_value(request, "CALLBACK");
    const wifi_tool_t::span_text timeout_span = wifi_tool_t::find_http_header_value(request, "TIMEOUT");

    const bool has_sid = sid_span.ptr != nullptr &&
        wifi_tool_t::copy_str(sid, sizeof(sid), sid_span.ptr, sid_span.len) == ERRCODE_SUCC;
    const bool has_callback = callback_span.ptr != nullptr &&
        wifi_tool_t::copy_str(callback, sizeof(callback), callback_span.ptr, callback_span.len) == ERRCODE_SUCC;
    const bool has_timeout = timeout_span.ptr != nullptr &&
        wifi_tool_t::copy_str(timeout_text, sizeof(timeout_text), timeout_span.ptr, timeout_span.len) == ERRCODE_SUCC;
    if (has_callback) {
        (void)wifi_tool_t::trim_and_strip(callback, '<', '>');
    }
    if (has_timeout && wifi_tool_t::is_strstr_ignore_case(timeout_text, "Second-")) {
        const char *p = wifi_tool_t::strstr_ignore_case(timeout_text, "Second-");
        if (p != nullptr) {
            timeout_sec = static_cast<uint32_t>(atoi(p + 7));
        }
    }

    dlna_service_kind kind = dlna_service_kind::unknown;
    if (strstr(request, "SUBSCRIBE /AVTransport/event") != nullptr) {
        kind = dlna_service_kind::avtransport;
    } else if (strstr(request, "SUBSCRIBE /RenderingControl/event") != nullptr) {
        kind = dlna_service_kind::rendering_control;
    } else if (strstr(request, "SUBSCRIBE /ConnectionManager/event") != nullptr) {
        kind = dlna_service_kind::connection_manager;
    }

    if (kind == dlna_service_kind::unknown || (!has_sid && !has_callback)) {
        out->http_status = 400;
        (void)snprintf(out->status_text, sizeof(out->status_text), "Bad Request");
        (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
        (void)snprintf(out->body, sizeof(out->body), "invalid subscribe");
        return ERRCODE_FAIL;
    }

    // SUBSCRIBE 有两种形态：首次订阅带 CALLBACK，续租只带 SID。
    const bool is_renew = has_sid && !has_callback;
    if (is_renew) {
        subscribe_slot *slot = subs->get(kind);
        // 续租只能作用于当前已存在的活动订阅，避免无效 SID 覆盖正常状态。
        if (slot == nullptr || !slot->active || !wifi_tool_t::strcmp_ignore_case(slot->sid, sid)) {
            out->http_status = 412;
            (void)snprintf(out->status_text, sizeof(out->status_text), "Precondition Failed");
            (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
            (void)snprintf(out->body, sizeof(out->body), "invalid sid");
            return ERRCODE_FAIL;
        }
        slot->timeout_sec = (timeout_sec == 0) ? slot->timeout_sec : timeout_sec;
    } else {
        // 首次订阅时创建或覆盖槽位，并把“初始事件待发送”标志带给 renderer。
        const char *sid_to_use = has_sid ? sid : "uuid:ws63-dlna-default-sub";
        subs->upsert(kind, sid_to_use, callback, timeout_sec);
        if (kind == dlna_service_kind::avtransport) {
            out->send_initial_avtransport_notify = true;
        } else if (kind == dlna_service_kind::rendering_control) {
            out->send_initial_rendering_notify = true;
        }
    }

    out->http_status = 200;
    (void)snprintf(out->status_text, sizeof(out->status_text), "OK");
    (void)snprintf(out->content_type, sizeof(out->content_type), "text/plain");
    out->body[0] = '\0';
    return ERRCODE_SUCC;
}
```

注意：

- `ConnectionManager` 的 event 先预留接口即可，当前不需要真的发复杂状态
- 初次订阅才触发 `send_initial_avtransport_notify` / `send_initial_rendering_notify`；续租只更新超时，不重发初始事件
- `send_initial_avtransport_notify` 这种标志不要在这里直接发网包，由 `media_renderer` 回复 HTTP 200 后再触发 `notify_sender`

### 9.6 为什么控制路由一定要独立

独立后有三个直接收益：

- 以后你要补动作，只改 router，不碰 socket 生命周期
- 以后你要换 XML 构造方式，只改 xml，不碰播放控制
- 以后你要把 `play` 内部从 MP3 流播放器换成别的实现，不碰 SOAP 解析

---

## 10. ssdp 模块

### 10.1 职责说明

SSDP 模块只做三件事：

- 建立 1900/UDP 套接字并加入组播
- 接收 M-SEARCH
- 针对合法 ST 回复对应响应

它不应该知道 `CurrentURI`、`Play`、`Volume` 这些控制面状态。

### 10.2 `includes/wlan/dlna/ssdp/ssdp_service.hpp`

```cpp
#pragma once

extern "C" {
#include "lwip/sockets.h"
#include "lwip/igmp.h"
#include "netinet/in.h"
#include "common_def.h"
}

#include <cstdint>

namespace sed_ws63 {

class ssdp_service {
public:
    ssdp_service() = default;
    ~ssdp_service();

    errcode_t open_socket();
    errcode_t process_once(const char *local_ip, uint16_t http_port, const char *udn);
    void close();
    int32_t fd() const;

private:
    errcode_t send_reply_(const sockaddr_in &peer, socklen_t peer_len,
                          const char *local_ip, uint16_t http_port,
                          const char *udn, const char *st, const char *usn_suffix = nullptr);

private:
    int32_t sfd_ = -1;
    static constexpr uint16_t ssdp_port_ = 1900;
    static constexpr const char *mcast_ip_ = "239.255.255.250";
};

} // namespace sed_ws63
```

### 10.3 `includes/wlan/dlna/ssdp/ssdp_service.cpp`

```cpp
#include "ssdp_service.hpp"

#include "wifi_tool.hpp"

namespace sed_ws63 {

using wifi_tool_t = sed_ws63::wifi_tool;

ssdp_service::~ssdp_service()
{
    close();
}

errcode_t ssdp_service::open_socket()
{
    close();

    // SSDP 使用独立 UDP socket，避免和业务层普通 UDP 封装耦合。
    sfd_ = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd_ < 0) {
        return ERRCODE_FAIL;
    }

    int32_t reuse = 1;
    (void)lwip_setsockopt(sfd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = lwip_htons(ssdp_port_);
    if (lwip_bind(sfd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close();
        return ERRCODE_FAIL;
    }

    // 绑定成功后再加入 SSDP 固定多播组，后续才能收到 M-SEARCH。
    ip_mreq mreq = {};
    mreq.imr_multiaddr.s_addr = inet_addr(mcast_ip_);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (lwip_setsockopt(sfd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        close();
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCC;
}

errcode_t ssdp_service::process_once(const char *local_ip, uint16_t http_port, const char *udn)
{
    if (sfd_ < 0 || local_ip == nullptr || udn == nullptr) {
        return ERRCODE_FAIL;
    }

    // 每次只处理一个 UDP 报文，保持 renderer 主循环简单可控。
    char buffer[1024] = {0};
    sockaddr_in peer = {};
    socklen_t peer_len = sizeof(peer);
    int ret = lwip_recvfrom(sfd_, buffer, sizeof(buffer) - 1, 0,
                            reinterpret_cast<sockaddr *>(&peer), &peer_len);
    if (ret <= 0) {
        return ERRCODE_FAIL;
    }
    buffer[ret] = '\0';

    if (strstr(buffer, "M-SEARCH") == nullptr) {
        return ERRCODE_SUCC;
    }

    // 只接受合法的 M-SEARCH，避免把普通组播报文误当作发现请求。
    char st[128] = {0};
    char man[128] = {0};
    char host[64] = {0};
    const wifi_tool_t::span_text st_span = wifi_tool_t::find_http_header_value(buffer, "ST");
    const wifi_tool_t::span_text man_span = wifi_tool_t::find_http_header_value(buffer, "MAN");
    const wifi_tool_t::span_text host_span = wifi_tool_t::find_http_header_value(buffer, "HOST");
    if (st_span.ptr == nullptr || man_span.ptr == nullptr ||
        wifi_tool_t::copy_str(st, sizeof(st), st_span.ptr, st_span.len) != ERRCODE_SUCC ||
        wifi_tool_t::copy_str(man, sizeof(man), man_span.ptr, man_span.len) != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }
    if (host_span.ptr != nullptr) {
        (void)wifi_tool_t::copy_str(host, sizeof(host), host_span.ptr, host_span.len);
    }
    (void)wifi_tool_t::trim(st);
    (void)wifi_tool_t::trim(man);
    (void)wifi_tool_t::trim(host);

    if (!wifi_tool_t::is_strstr_ignore_case(man, "ssdp:discover")) {
        return ERRCODE_FAIL;
    }
    if (host[0] != '\0' && strstr(host, "239.255.255.250:1900") == nullptr) {
        return ERRCODE_FAIL;
    }

    const bool is_ssdp_all = wifi_tool_t::strcmp_ignore_case(st, "ssdp:all");
    const bool is_root = wifi_tool_t::strcmp_ignore_case(st, "upnp:rootdevice");
    const bool is_uuid = wifi_tool_t::strcmp_ignore_case(st, udn);
    const bool is_renderer = wifi_tool_t::strcmp_ignore_case(st, "urn:schemas-upnp-org:device:MediaRenderer:1");
    const bool is_avt = wifi_tool_t::strcmp_ignore_case(st, "urn:schemas-upnp-org:service:AVTransport:1");
    const bool is_rcs = wifi_tool_t::strcmp_ignore_case(st, "urn:schemas-upnp-org:service:RenderingControl:1");
    const bool is_cm = wifi_tool_t::strcmp_ignore_case(st, "urn:schemas-upnp-org:service:ConnectionManager:1");

    if (!(is_ssdp_all || is_root || is_uuid || is_renderer || is_avt || is_rcs || is_cm)) {
        return ERRCODE_SUCC;
    }

    if (is_ssdp_all) {
        // ssdp:all 需要逐条回复多个目标，不要偷懒合并成一条。
        (void)send_reply_(peer, peer_len, local_ip, http_port, udn, "upnp:rootdevice", "upnp:rootdevice");
        (void)send_reply_(peer, peer_len, local_ip, http_port, udn, udn);
        (void)send_reply_(peer, peer_len, local_ip, http_port, udn,
                          "urn:schemas-upnp-org:device:MediaRenderer:1",
                          "urn:schemas-upnp-org:device:MediaRenderer:1");
        (void)send_reply_(peer, peer_len, local_ip, http_port, udn,
                          "urn:schemas-upnp-org:service:AVTransport:1",
                          "urn:schemas-upnp-org:service:AVTransport:1");
        (void)send_reply_(peer, peer_len, local_ip, http_port, udn,
                          "urn:schemas-upnp-org:service:RenderingControl:1",
                          "urn:schemas-upnp-org:service:RenderingControl:1");
        (void)send_reply_(peer, peer_len, local_ip, http_port, udn,
                          "urn:schemas-upnp-org:service:ConnectionManager:1",
                          "urn:schemas-upnp-org:service:ConnectionManager:1");
        return ERRCODE_SUCC;
    }

    if (is_root) {
        return send_reply_(peer, peer_len, local_ip, http_port, udn, st, "upnp:rootdevice");
    }
    if (is_uuid) {
        return send_reply_(peer, peer_len, local_ip, http_port, udn, st);
    }
    return send_reply_(peer, peer_len, local_ip, http_port, udn, st, st);
}

errcode_t ssdp_service::send_reply_(const sockaddr_in &peer, socklen_t peer_len,
                                    const char *local_ip, uint16_t http_port,
                                    const char *udn, const char *st, const char *usn_suffix)
{
    char usn[256] = {0};
    // 设备 UUID 响应和带服务后缀的 USN 格式不同，这里统一拼装。
    if (usn_suffix != nullptr && usn_suffix[0] != '\0' && !wifi_tool_t::strcmp_ignore_case(st, udn)) {
        (void)snprintf(usn, sizeof(usn), "%s::%s", udn, usn_suffix);
    } else {
        (void)snprintf(usn, sizeof(usn), "%s", udn);
    }

    char response[896] = {0};
    (void)snprintf(response, sizeof(response),
                   "HTTP/1.1 200 OK\r\n"
                   "CACHE-CONTROL: max-age=1800\r\n"
                   "EXT:\r\n"
                   "LOCATION: http://%s:%u/description.xml\r\n"
                   "SERVER: Linux/5.10 UPnP/1.1 WS63/1.0\r\n"
                   "ST: %s\r\n"
                   "USN: %s\r\n\r\n",
                   local_ip, http_port, st, usn);

    if (lwip_sendto(sfd_, response, strlen(response), 0,
                    reinterpret_cast<const sockaddr *>(&peer), peer_len) <= 0) {
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCC;
}

void ssdp_service::close()
{
    if (sfd_ >= 0) {
        lwip_close(sfd_);
        sfd_ = -1;
    }
}

int32_t ssdp_service::fd() const
{
    return sfd_;
}

} // namespace sed_ws63
```

### 10.4 为什么 SSDP 不建议直接复用当前 udp 类

当前 `includes/wlan/udp/udp.hpp` 的职责是普通 UDP 读写，不负责：

- 组播加入
- SSDP 固定头校验
- 多目标 ST 响应

所以第一阶段更建议让 `ssdp_service` 自己管理这个 socket。等你以后要做更多组播协议时，再考虑抽一个 multicast_udp 基础类。

---

## 11. playback_bridge 模块

### 11.1 为什么播放逻辑建议单独开一个模块

你提到“播放逻辑可以单独开一个文件”，这个建议是对的。

但要注意，这里说的“播放逻辑”有两层：

- 控制面桥接：`SetURI / Play / Pause / Stop` 这些命令如何发给播放器
- 数据面实现：真正的拉流、解码、节奏控制

DLNA 重写的第一阶段，至少要把“控制面桥接”独立出来。这样 `control_router` 不会再直接调旧自研播放器的静态函数，而是只依赖新的 `play` 服务接口。

### 11.2 `includes/wlan/dlna/playback_bridge/playback_bridge.hpp`

```cpp
#pragma once

#include "dlna_types/dlna_types.hpp"

namespace sed_ws63 {

class playback_bridge {
public:
    using set_uri_handler = errcode_t (*)(const char *uri, const char *metadata);
    using simple_handler = errcode_t (*)();

    void set_uri_handler_set(set_uri_handler handler);
    void play_handler_set(simple_handler handler);
    void pause_handler_set(simple_handler handler);
    void stop_handler_set(simple_handler handler);

    errcode_t dispatch(const playback_command &cmd);
    errcode_t set_uri(const char *uri, const char *metadata);
    errcode_t play();
    errcode_t pause();
    errcode_t stop();

private:
    set_uri_handler set_uri_handler_ = nullptr;
    simple_handler play_handler_ = nullptr;
    simple_handler pause_handler_ = nullptr;
    simple_handler stop_handler_ = nullptr;
};

} // namespace sed_ws63
```

### 11.3 `includes/wlan/dlna/playback_bridge/playback_bridge.cpp`

```cpp
#include "playback_bridge.hpp"

namespace sed_ws63 {

void playback_bridge::set_uri_handler_set(set_uri_handler handler)
{
    set_uri_handler_ = handler;
}

void playback_bridge::play_handler_set(simple_handler handler)
{
    play_handler_ = handler;
}

void playback_bridge::pause_handler_set(simple_handler handler)
{
    pause_handler_ = handler;
}

void playback_bridge::stop_handler_set(simple_handler handler)
{
    stop_handler_ = handler;
}

errcode_t playback_bridge::dispatch(const playback_command &cmd)
{
    switch (cmd.type) {
        case playback_command_type::set_uri:
            return set_uri(cmd.uri, cmd.metadata);
        case playback_command_type::play:
            return play();
        case playback_command_type::pause:
            return pause();
        case playback_command_type::stop:
            return stop();
        default:
            return ERRCODE_FAIL;
    }
}

errcode_t playback_bridge::set_uri(const char *uri, const char *metadata)
{
    if (set_uri_handler_ == nullptr) {
        return ERRCODE_FAIL;
    }
    return set_uri_handler_(uri, metadata);
}

errcode_t playback_bridge::play()
{
    return (play_handler_ == nullptr) ? ERRCODE_FAIL : play_handler_();
}

errcode_t playback_bridge::pause()
{
    return (pause_handler_ == nullptr) ? ERRCODE_FAIL : pause_handler_();
}

errcode_t playback_bridge::stop()
{
    return (stop_handler_ == nullptr) ? ERRCODE_FAIL : stop_handler_();
}

} // namespace sed_ws63
```

### 11.4 为什么这比旧的全局静态回调更好

旧的 `dlan::register_media_*_handler()` 有两个问题：

- 它本质上还是把状态挂在全局静态上
- `dlan` 这个类承担了不属于它的桥接职责

拆成 `playback_bridge` 后：

- `control_router` 只依赖一个 bridge 对象
- 应用层可以自由把它接到 `play` 服务、本地文件播放器，甚至未来的 AAC/FLAC 播放链路
- 以后你要做测试桩，只要注册一组 fake handler 即可

---

## 12. media_renderer 编排层

### 12.1 职责说明

`media_renderer` 相当于 WiFi 重构里的 `provisioner`，它是总编排层。

它负责：

- 等待拿到有效 IP
- 启动 SSDP 和 HTTP listener
- 进入 select 循环
- 收到 HTTP 请求后交给 `control_router`
- 在 SUBSCRIBE 成功后触发初始 NOTIFY

它不直接写 XML，也不直接调用 `play` 的 HTTP/解码细节。

### 12.2 `includes/wlan/dlna/renderer/media_renderer.hpp`

```cpp
#pragma once

#include <cstddef>

#include "dlna_types/dlna_types.hpp"
#include "event/subscription_manager.hpp"
#include "event/notify_sender.hpp"
#include "playback_bridge/playback_bridge.hpp"
#include "soap/control_router.hpp"
#include "ssdp/ssdp_service.hpp"
#include "tcp_listener.hpp"

extern "C" {
#include "lwip/netif.h"
}

namespace sed_ws63 {

class media_renderer {
public:
    explicit media_renderer(playback_bridge &bridge);
    errcode_t run();

private:
    errcode_t wait_valid_ip_(char *out_ip, size_t out_size);
    errcode_t handle_http_client_(int32_t client_fd);
    errcode_t send_initial_notify_if_needed_(const control_response &resp);

private:
    playback_bridge &bridge_;
    ssdp_service ssdp_;
    tcp_listener http_listener_;
    control_router router_;
    subscription_manager subscriptions_;
    notify_sender notifier_;
    renderer_state state_;
    char local_ip_[16] = {0};

    static constexpr uint16_t http_port_ = 49152;
    static constexpr const char *udn_ = "uuid:20260321-1612-2007-0423-a1b2c3d4e5f6";
};

} // namespace sed_ws63
```

### 12.3 `includes/wlan/dlna/renderer/media_renderer.cpp`

```cpp
#include "media_renderer.hpp"
#include "xml/xml_builder.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "soc_osal.h"
#include "lwip/netifapi.h"
}

namespace sed_ws63 {
namespace {

int build_http_response_packet(char *out, size_t out_size, const control_response &resp, const char *sid_header = nullptr)
{
    const int body_len = static_cast<int>(strlen(resp.body));

    // 只有 SUBSCRIBE 这类响应才需要把 SID 回写给控制点。
    if (sid_header != nullptr && sid_header[0] != '\0') {
        return snprintf(out, out_size,
                        "HTTP/1.1 %d %s\r\n"
                        "CONTENT-TYPE: %s\r\n"
                        "SID: %s\r\n"
                        "TIMEOUT: Second-1800\r\n"
                        "CONTENT-LENGTH: %d\r\n"
                        "CONNECTION: close\r\n\r\n"
                        "%s",
                        resp.http_status, resp.status_text, resp.content_type, sid_header, body_len, resp.body);
    }

    return snprintf(out, out_size,
                    "HTTP/1.1 %d %s\r\n"
                    "CONTENT-TYPE: %s\r\n"
                    "CONTENT-LENGTH: %d\r\n"
                    "CONNECTION: close\r\n\r\n"
                    "%s",
                    resp.http_status, resp.status_text, resp.content_type, body_len, resp.body);
}

} // namespace

media_renderer::media_renderer(playback_bridge &bridge)
    : bridge_(bridge), state_(make_default_renderer_state())
{
}

errcode_t media_renderer::wait_valid_ip_(char *out_ip, size_t out_size)
{
    if (out_ip == nullptr || out_size == 0) {
        return ERRCODE_FAIL;
    }

    // WiFi 连上不等于 DHCP 完成，这里一直等到拿到非 0 地址再开放服务。
    while (true) {
        netif *netif_p = netif_default;
        if (netif_p != nullptr && netif_is_up(netif_p)) {
            uint32_t ip_host_order = lwip_ntohl(netif_p->ip_addr.u_addr.ip4.addr);
            (void)snprintf(out_ip, out_size, "%u.%u.%u.%u",
                           (ip_host_order >> 24) & 0xFF,
                           (ip_host_order >> 16) & 0xFF,
                           (ip_host_order >> 8) & 0xFF,
                           ip_host_order & 0xFF);
            if (strcmp(out_ip, "0.0.0.0") != 0) {
                return ERRCODE_SUCC;
            }
        }
        osal_msleep(500);
    }
}

errcode_t media_renderer::run()
{
    if (wait_valid_ip_(local_ip_, sizeof(local_ip_)) != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }

    // 先把发现面和控制面监听都拉起来，再进入统一的事件循环。
    if (ssdp_.open_socket() != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }
    if (http_listener_.listen_on(http_port_) != ERRCODE_SUCC) {
        return ERRCODE_FAIL;
    }

    while (true) {
        // renderer 自己只做多路复用和调度，不在这里展开协议细节。
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(ssdp_.fd(), &read_fds);
        FD_SET(http_listener_.fd(), &read_fds);

        const int max_fd = (ssdp_.fd() > http_listener_.fd()) ? ssdp_.fd() : http_listener_.fd();
        if (lwip_select(max_fd + 1, &read_fds, nullptr, nullptr, nullptr) < 0) {
            continue;
        }

        if (FD_ISSET(ssdp_.fd(), &read_fds)) {
            (void)ssdp_.process_once(local_ip_, http_port_, udn_);
        }

        if (FD_ISSET(http_listener_.fd(), &read_fds)) {
            sockaddr_in peer = {};
            int32_t client_fd = http_listener_.accept_one(&peer);
            if (client_fd >= 0) {
                (void)handle_http_client_(client_fd);
            }
        }
    }

    return ERRCODE_SUCC;
}

errcode_t media_renderer::handle_http_client_(int32_t client_fd)
{
    char request[2048] = {0};
    int ret = lwip_recv(client_fd, request, sizeof(request) - 1, 0);
    if (ret <= 0) {
        lwip_close(client_fd);
        return ERRCODE_FAIL;
    }
    request[ret] = '\0';

    // HTTP 解析与 SOAP 分发全部交给 router，renderer 只保留调度职责。
    control_response resp = make_default_control_response();
    (void)router_.handle_request(request, local_ip_, http_port_, udn_, &state_, &subscriptions_, &bridge_, &resp);

    const subscribe_slot *avt = subscriptions_.get(dlna_service_kind::avtransport);
    const subscribe_slot *rcs = subscriptions_.get(dlna_service_kind::rendering_control);
    const subscribe_slot *cm = subscriptions_.get(dlna_service_kind::connection_manager);
    const char *sid_header = nullptr;
    // 只有订阅响应需要回写 SID，普通 GET/POST 不需要额外头。
    if (strncmp(request, "SUBSCRIBE /AVTransport/event", 28) == 0 && avt != nullptr) {
        sid_header = avt->sid;
    } else if (strncmp(request, "SUBSCRIBE /RenderingControl/event", 33) == 0 && rcs != nullptr) {
        sid_header = rcs->sid;
    } else if (strncmp(request, "SUBSCRIBE /ConnectionManager/event", 34) == 0 && cm != nullptr) {
        sid_header = cm->sid;
    }

    char packet[3072] = {0};
    int packet_len = build_http_response_packet(packet, sizeof(packet), resp, sid_header);
    if (packet_len > 0) {
        (void)lwip_send(client_fd, packet, packet_len, 0);
    }
    lwip_close(client_fd);

    return send_initial_notify_if_needed_(resp);
}

errcode_t media_renderer::send_initial_notify_if_needed_(const control_response &resp)
{
    if (resp.send_initial_avtransport_notify) {
        subscribe_slot *slot = subscriptions_.get(dlna_service_kind::avtransport);
        if (slot != nullptr && slot->active) {
            char body[1024] = {0};
            // 初始事件必须在 HTTP 200 回完之后再发，避免控制点还没完成订阅就收到 NOTIFY。
            uint32_t seq = subscriptions_.next_seq(dlna_service_kind::avtransport);
            (void)build_avtransport_event_xml(body, sizeof(body), state_);
            (void)notifier_.send_event(*slot, seq, body);
        }
    }

    if (resp.send_initial_rendering_notify) {
        subscribe_slot *slot = subscriptions_.get(dlna_service_kind::rendering_control);
        if (slot != nullptr && slot->active) {
            char body[1024] = {0};
            uint32_t seq = subscriptions_.next_seq(dlna_service_kind::rendering_control);
            (void)build_rendering_event_xml(body, sizeof(body), state_);
            (void)notifier_.send_event(*slot, seq, body);
        }
    }

    return ERRCODE_SUCC;
}

} // namespace sed_ws63
```

### 12.4 为什么编排层要单独存在

如果没有这层，最后还是会回到旧 dlan.cpp 那种结构：

- HTTP 路由里自己调用 `send()`
- SUBSCRIBE 路由里自己发 NOTIFY
- SSDP 和 SOAPAction 都改同一份静态状态

有了 `media_renderer` 以后，职责明确变成：

- `router` 只决定“回什么”
- `renderer` 决定“什么时候收、什么时候发、谁来发”

---

## 13. 应用层使用示例

### 13.1 第一阶段最稳妥的接法

这次不再保留 `minimp3::prepare_url()`、`minimp3::play_url()` 这类旧自研播放器接口作为应用层核心路径。应用层应该直接把 `playback_bridge` 绑定到新的 `play` 服务入口上。

### 13.2 示例：直接在 `wifi_task.cpp` 里组装

```cpp
#include "renderer/media_renderer.hpp"
#include "playback_bridge/playback_bridge.hpp"
#include "play/play_service.hpp"

namespace {

errcode_t dlna_set_uri(const char *uri, const char *metadata)
{
    return sed_ws63::play_set_uri(uri, metadata);
}

errcode_t dlna_play()
{
    return sed_ws63::play_start();
}

errcode_t dlna_pause()
{
    return sed_ws63::play_pause();
}

errcode_t dlna_stop()
{
    return sed_ws63::play_stop();
}

} // namespace

void *wifi_task(void *arg)
{
    unused(arg);

    while (wifi_is_wifi_inited() == 0) {
        osal_msleep(100);
    }

    sed_ws63::playback_bridge bridge;
    bridge.set_uri_handler_set(dlna_set_uri);
    bridge.play_handler_set(dlna_play);
    bridge.pause_handler_set(dlna_pause);
    bridge.stop_handler_set(dlna_stop);

    sed_ws63::media_renderer renderer(bridge);
    (void)renderer.run();
    return nullptr;
}
```

### 13.3 如果你想把适配器单独放到 `realize/audio_play/`

这比把绑定代码留在 `wifi_task.cpp` 更干净，推荐新增：

```text
realize/audio_play/
├── dlna_playback_adapter.hpp
├── dlna_playback_adapter.cpp
└── play_task.cpp
```

接口可以非常简单：

```cpp
#pragma once

#include "playback_bridge/playback_bridge.hpp"

namespace sed_ws63 {

void bind_play_to_bridge(playback_bridge &bridge);

} // namespace sed_ws63
```

这样分层以后：

- `wifi_task.cpp` 只负责联网与启动 `media_renderer`
- `dlna_playback_adapter.cpp` 只负责把 `SetURI / Play / Pause / Stop` 映射到 `play_set_uri() / play_start() / play_pause() / play_stop()`
- `play_task.cpp` 只负责挂接 IIS、PCM 节奏控制，并启动 `play_run_loop()`

---

## 14. play 播放链路模块

### 14.1 为什么这次要直接改成 `play/`

这次不是把旧 `minimp3.cpp` 改几行，而是要把它从架构上移除。原因有三个：

1. `minimp3` 这个名字在你当前仓库里同时指代“自研播放器类”和“第三方 miniMP3 解码库”，命名已经失真。
2. 旧自研 `minimp3.cpp` 把 URL 状态、HTTP 拉流、ICY 过滤、格式探测、解码驱动、PCM 推送全部揉进一个类，已经没有继续演进的价值。
3. 你已经明确要求重写自己的播放器实现，因此最干净的做法就是新建 `play/`，把旧自研 `minimp3.cpp/minimp3.hpp` 直接替掉。

必须强调：

- 要删除的是你自己写的 `minimp3.cpp/minimp3.hpp`
- 要保留的是第三方 `miniMP3/minimp3.h`
- 第三方库以后只在 `play/mp3_decoder.cpp` 内部出现，不再作为工程对外接口

### 14.2 推荐结构

```text
includes/play/
├── play_service.hpp            ← 对 DLNA / 应用层暴露的稳定入口
├── play_service.cpp
├── playback_session.hpp        ← current_uri / play / pause / stop 状态
├── playback_session.cpp
├── http_stream_reader.hpp      ← TCP 建连、GET、响应头解析、ICY metadata 过滤
├── http_stream_reader.cpp
├── format_probe.hpp            ← 非 MP3 签名探测、ID3 跳过、sync 搜索
├── format_probe.cpp
├── mp3_decoder.hpp             ← 只负责 bytes -> PCM
└── mp3_decoder.cpp             ← 内部 include miniMP3/minimp3.h

realize/audio_play/
├── pcm_pacer.hpp               ← IIS 队列闭环节奏控制
├── pcm_pacer.cpp
├── dlna_playback_adapter.hpp   ← playback_bridge -> play_service
├── dlna_playback_adapter.cpp
└── play_task.cpp               ← 配置 IIS 回调并启动 play_run_loop()
```

### 14.3 每个文件只负责什么

| 文件 | 职责 |
|---|---|
| `play_service.*` | 对外提供 `set_uri / start / pause / stop / run_loop` 五类接口 |
| `playback_session.*` | 保存 URL、播放请求、暂停态、代际号等会话状态 |
| `http_stream_reader.*` | 建立 socket、发送 GET、读取 HTTP 头、过滤 ICY metadata |
| `format_probe.*` | 判断明显非 MP3 数据、跳过 ID3、做 MP3 sync 重定位 |
| `mp3_decoder.*` | 封装第三方 miniMP3，把压缩字节流解码成 PCM |
| `pcm_pacer.*` | 根据 IIS 队列水位做节流和补帧 |
| `dlna_playback_adapter.*` | 把 `playback_bridge` 的四个动作映射到 `play_service` |
| `play_task.cpp` | 初始化 IIS、注册 PCM 输出回调、启动 `play_run_loop()` |

### 14.4 `play_service` 对外接口建议

`play_service` 的目的就是把 DLNA、应用层、解码器彻底隔开。对外只暴露下面这些接口：

```cpp
#pragma once

extern "C" {
#include "common_def.h"
}

#include <cstdint>

namespace sed_ws63 {

using play_output_rate_handler = void (*)(int rate);
using play_pcm_output_handler = void (*)(const int16_t *data, uint32_t size);
using play_queue_level_getter = int (*)();

void play_configure_output_rate(play_output_rate_handler handler);
void play_configure_pcm_output(play_pcm_output_handler handler);
void play_configure_queue_level_getter(play_queue_level_getter getter);

errcode_t play_set_uri(const char *uri, const char *metadata = nullptr);
errcode_t play_start();
errcode_t play_pause();
errcode_t play_stop();

void play_run_loop();

} // namespace sed_ws63
```

这样做之后：

- DLNA 只知道 `set_uri / start / pause / stop`
- `wifi_task.cpp` 不再碰 socket 和解码细节
- `play_task.cpp` 不再关心 SOAPAction

### 14.5 miniMP3 外部库应该怎样保留

第三方 miniMP3 库仍然保留，但只能作为 `mp3_decoder.cpp` 的内部实现细节。推荐边界如下：

```cpp
#define MINIMP3_IMPLEMENTATION
#include "miniMP3/minimp3.h"
```

边界要求必须明确：

- 不再保留 `class minimp3`
- 不再让 `wifi_task.cpp`、`dlan.cpp`、`control_router.cpp` 直接 include `miniMP3/minimp3.h`
- `mp3_decoder` 只接收压缩字节流并输出 PCM，不持有 URL、socket、播放标志位

### 14.6 现有旧代码应该如何落位

| 当前函数/逻辑 | 新位置 |
|---|---|
| `minimp3.hpp` 对外控制接口 | `play/play_service.hpp` |
| `http_set_url()` / `http_get_url()` / `http_stop()` / `http_clear_url()` | `play/playback_session.cpp` |
| 拉流建连、HTTP 请求、响应头读取、ICY metadata 过滤 | `play/http_stream_reader.cpp` |
| `has_known_non_mp3_signature()` | `play/format_probe.cpp` |
| `parse_id3v2_tag_total_size_if_present()` | `play/format_probe.cpp` |
| `find_mp3_sync_offset()` | `play/format_probe.cpp` |
| `classify_payload_prefix()` / `log_payload_preview()` | `play/format_probe.cpp` |
| `mp3dec_decode_frame()` 驱动与采样率切换 | `play/mp3_decoder.cpp` |
| `stream_mp3_to_iis()` 主循环 | `play/play_service.cpp` |
| `push_pcm_with_closed_loop()` | `realize/audio_play/pcm_pacer.cpp` |

### 14.7 为什么这对 DLNA 重写是必须的

因为一旦控制面重写完成，你马上会遇到这两个要求：

- `Pause` 想做成真暂停，必须把播放会话状态从旧自研 `minimp3` 类里独立出来
- `SetURI` 和 `Play` 想做到更稳定，必须让控制面和数据面之间只传命令，不共享静态全局状态

所以这次不应该再把它定义成“第二阶段再说”，而应该直接在教程里把 `play/` 作为正式目标结构写清楚。

---

## 15. CMakeLists.txt 更新方案

### 15.1 `main/receiving end/includes/CMakeLists.txt`

这次要先把新的 `play/` 子目录接进来：

```cmake
add_subdirectory_if_exist(play)
add_subdirectory_if_exist(wlan)
add_subdirectory_if_exist(miniMP3)
```

### 15.2 新建 `includes/play/CMakeLists.txt`

```cmake
set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/play_service.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/playback_session.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/http_stream_reader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/format_probe.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/mp3_decoder.cpp"
    PARENT_SCOPE)
```

### 15.3 `includes/miniMP3/CMakeLists.txt`

这里要收缩而不是继续编译旧自研 `minimp3.cpp`。推荐做法是：

```cmake
set(SOURCES "${SOURCES}" PARENT_SCOPE)
```

也就是说：

- `includes/miniMP3/miniMP3/minimp3.h` 保留
- 旧自研 `includes/miniMP3/minimp3.cpp` / `minimp3.hpp` 删除
- `MINIMP3_IMPLEMENTATION` 挪到 `play/mp3_decoder.cpp`

### 15.4 `includes/wlan/CMakeLists.txt`

你当前这个文件已经有：

```cmake
add_subdirectory_if_exist(tcp)
add_subdirectory_if_exist(dlna)
```

因此这一层不需要再改。

### 15.5 新建 `includes/wlan/tcp/CMakeLists.txt`

```cmake
set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tcp_client.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tcp_listener.cpp"
    PARENT_SCOPE)
```

### 15.6 新建 `includes/wlan/dlna/CMakeLists.txt`

```cmake
add_subdirectory_if_exist(dlna_types)
add_subdirectory_if_exist(xml)
add_subdirectory_if_exist(event)
add_subdirectory_if_exist(soap)
add_subdirectory_if_exist(ssdp)
add_subdirectory_if_exist(playback_bridge)
add_subdirectory_if_exist(renderer)

set(SOURCES "${SOURCES}" PARENT_SCOPE)
```

### 15.7 各子模块 `CMakeLists.txt`

每个子模块都按下面这种最小形式写即可，例如 `includes/wlan/dlna/xml/CMakeLists.txt`：

```cmake
set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/xml_text.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/xml_builder.cpp"
    PARENT_SCOPE)
```

`event`、`soap`、`ssdp`、`playback_bridge`、`renderer` 同理。

### 15.8 `main/receiving end/CMakeLists.txt` 是否需要改

本教程采用的目录布局有一个好处：

- 你当前已经把 `main/receiving end/includes` 加到了头文件搜索路径
- `play/` 是 `includes/` 的一级子目录，因此 `#include "play/play_service.hpp"` 可以直接工作
- XML 仍然是 `dlna/xml/` 子目录，不是新的顶层模块

因此只要你按本教程的 include 风格写：

```cpp
#include "play/play_service.hpp"
#include "renderer/media_renderer.hpp"
#include "soap/control_router.hpp"
#include "xml/xml_builder.hpp"
```

就不需要额外修改 `main/receiving end/CMakeLists.txt`。

### 15.9 `realize/audio_play/CMakeLists.txt`

你既然已经决定正式引入 `play/`，那么音频侧建议把下面几个实现一起接进来：

```cmake
set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/audio_play.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/pcm_pacer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/dlna_playback_adapter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/play_task.cpp"
    PARENT_SCOPE)
```

---

## 16. 原 dlan 与旧播放链路功能对照映射表

### 16.1 dlan 旧功能映射

| 原位置 | 新位置 |
|---|---|
| `dlan::ssdp_and_http_scan()` | `renderer/media_renderer::run()` |
| `dlan::ssdp_set()` | `ssdp/ssdp_service::open_socket()` |
| `dlan::ssdp_process()` | `ssdp/ssdp_service::process_once()` |
| `dlan::http_set()` | `tcp/tcp_listener::listen_on()` |
| `dlan::http_process()` | `renderer/media_renderer::handle_http_client_()` + `soap/control_router` |
| `send_http_notify_request()` | `event/notify_sender::send_event()` |
| `g_avt_callback/g_avt_sid/g_avt_seq` | `event/subscription_manager` 的 AVTransport 槽 |
| `g_rc_callback/g_rc_sid/g_rc_seq` | `event/subscription_manager` 的 RenderingControl 槽 |
| `g_current_uri` | `dlna_types::renderer_state.current_uri` |
| `g_transport_state` | `dlna_types::renderer_state.state` |
| `xml_escape_basic()` | `xml/xml_text.cpp` |
| 设备描述 XML 字符串 | `xml/xml_builder.cpp` |
| 各 service XML 字符串 | `xml/xml_builder.cpp` |
| `register_media_*_handler()` | `playback_bridge` |

### 16.2 旧自研 minimp3 播放链路映射

| 原位置 | 新位置 |
|---|---|
| `minimp3.hpp` 对外控制入口 | `play/play_service.hpp` |
| `http_set_url/http_get_url/http_stop/http_clear_url` | `play/playback_session.cpp` |
| 拉流建连与 HTTP 头读取 | `play/http_stream_reader.cpp` |
| MP3 格式探测和 sync 搜索 | `play/format_probe.cpp` |
| `stream_mp3_to_iis()` 主循环 | `play/play_service.cpp` |
| `push_pcm_with_closed_loop()` | `realize/audio_play/pcm_pacer.cpp` |
| `miniMP3/minimp3.h` | `play/mp3_decoder.cpp` 的内部依赖 |

---

## 17. 建议删除或下沉的旧代码

### 17.1 可以直接删除的内容

下面这些内容当前已经没有保留价值：

1. `dlan.cpp::stream_probe_once()`
2. `dlan.hpp::media_status` 与 `now_media_status`
3. 旧自研 `includes/miniMP3/minimp3.hpp`
4. 旧自研 `includes/miniMP3/minimp3.cpp`
5. `minimp3.cpp::parse_http_status_code_from_header()`，如果你在 `play/http_stream_reader.cpp` 内部单独保留了自己的版本
6. `minimp3.cpp::probe_mp3_frame_from_buffer()`，如果你不做预探测式启动

### 17.2 应下沉而不是删除的内容

下面这些逻辑是对的，只是位置不对：

1. `is_mp3_candidate_from_uri_or_metadata()` 下沉到 `control_router.cpp` 的 AVTransport 策略
2. `send_http_notify_request()` 下沉到 `notify_sender.cpp`
3. `send_http_soap_response()` / `send_http_soap_fault_response()` 下沉到 `control_router.cpp` 的本地响应写入辅助函数
4. `xml_escape_basic()` 下沉到 `xml_text.cpp`
5. `push_pcm_with_closed_loop()` 下沉到 `pcm_pacer.cpp`
6. 第三方 `miniMP3/minimp3.h` 下沉到 `play/mp3_decoder.cpp`，不再作为工程公共入口出现

### 17.3 状态层面必须收口的点

这几个“散落的状态”必须收口到对象成员中：

- `g_current_uri`
- `g_avt_callback`
- `g_rc_callback`
- `g_avt_seq`
- `g_rc_seq`
- `g_transport_state`
- `playback_session::current_uri_`
- `playback_session::play_requested_`
- `playback_session::paused_`
- `playback_session::generation_`

第一阶段至少要先解决前六个；后面四个属于新的 `play` 数据面实现，而且不应该再回到全局静态变量。

---

## 18. 常见问题与注意事项

### Q1：为什么 XML 不单独放到 `includes/wlan/xml/` 顶层目录？

可以，但当前不推荐。

原因有两个：

1. 你当前没有预留顶层 xml 目录入口
2. `main/receiving end/CMakeLists.txt` 已经把 `includes/wlan/dlna` 放进头文件搜索路径

所以第一阶段把 XML 放在 `includes/wlan/dlna/xml/` 最省改动，同时职责又足够独立。

### Q2：为什么这次必须新建 `play/`，而不是继续沿用 `minimp3.hpp`？

因为你当前的 `minimp3` 这个名字已经同时指向两件不同的东西：

1. 你自己写的播放器根类
2. 外部第三方 miniMP3 解码库

如果继续沿用这个名字，边界永远会混乱。最干净的做法就是：

- 自研播放器实现改名并迁到 `play/`
- 第三方解码库继续叫 `miniMP3`
- `mp3_decoder.cpp` 作为两者之间唯一接触点

### Q3：为什么这里还保留 miniMP3，而不是连解码库一起重写？

因为你现在要重写的是“播放链路架构”，不是“MP3 解码算法本身”。

当前最合理的边界是：

- 你自己实现 URL 状态、HTTP 拉流、格式探测、PCM 节奏、播放状态机
- 外部 miniMP3 只负责把 MP3 压缩帧解码成 PCM

这样重写的工作量和收益比才是合理的。

### Q4：Pause 现在并不是真暂停，这会不会影响本次模块化？

不会。

本次最重要的是保留 `pause()` 这个接口边界，不要把“当前 Pause 实际等价于 stop”写死在 DLNA 模块内部。临时兼容映射应放在应用层适配器里。

### Q5：为什么 `media_renderer` 里还保留了 select 循环？

因为这个循环本身就是编排层职责，它同时协调：

- SSDP UDP socket
- HTTP listener socket
- 收到请求后的分发

它应该存在，但不应该再带上 XML 文本拼接和播放命令直调。

### Q6：为什么不继续保留旧 `dlan` 类，只是把代码分文件？

因为那样只是“分文件”，不是“分职责”。

真正的问题不在文件数量，而在：

- 哪个对象拥有状态
- 哪个模块拥有 socket 生命周期
- 哪个模块拥有 XML 构造职责

如果继续保留一个巨大的 `dlan` 根类，你最后还是会把所有成员重新堆回去。

### Q7：当前 `SetAVTransportURI` 后是不是还要立即探测流可达性？

不建议。

当前仓库里已经有教训：额外的同步探测会和真实播放竞争连接，尤其对带签名 URL 的控制点更不稳定。第一阶段应该坚持：

- `SetURI` 只保存 URI
- `Play` 才启动真实播放

### Q8：`playback_bridge` 用函数指针是不是太简陋？

在当前 MCU 场景下，这个方案是足够好的。

优点：

- 简单
- 无额外 vtable 负担
- 容易和新的 `play_service` 对接

如果后续你要做 host 单元测试，再把它升级成纯虚接口也完全来得及。

### Q9：现在是否可以删除旧 `includes/dlan/dlan.cpp` 和 `includes/dlan/dlan.hpp`？

建议等新链路跑通后再删。

迁移顺序应该是：

1. 先按本教程把新模块建好
2. 应用层切到 `media_renderer`
3. 跑通发现、SetURI、Play、Stop
4. 再删除旧 dlan 文件

不要一上来先删旧代码，那样回归定位会很痛苦。

---

*文档版本：v3.0 | 日期：2026-05-04*