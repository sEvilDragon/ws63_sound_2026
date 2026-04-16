# WS63 DLNA 链路重构指南（模块化架构）

## 1. 目标与范围

这份文档是一份面向你当前 DLNA 播放链路的完整重构蓝图。

当前状态（基于你现有代码）：
- 控制面链路（DLNA/UPnP SOAP、SUBSCRIBE/NOTIFY）和底层 socket 细节混在一个大实现里。
- 数据面链路（HTTP 拉流、MP3 解码、IIS 输出）集中在一个超大的主循环中，局部状态非常多。
- 网络基础能力（TCP connect/send/recv、超时、重连）在多个位置重复实现。
- 业务状态（transport state、URL state、队列水位、playback state）跨模块泄漏，边界不清晰。

目标状态：
- 分层清晰，职责明确。
- 可复用的传输模块（TCP/HTTP/SSDP）独立于 DLNA 和解码逻辑。
- 控制面采用事件驱动，数据面采用流水线式处理。
- 通过渐进迁移降低回归风险。

第一阶段重构的非目标：
- 不扩展协议范围，先只支持现有的 HTTP MP3 拉流播放。
- 不扩展编解码器，先保留 minimp3。

---

## 2. 当前链路概览（现状）

当前运行链路大致如下：
1. app_entry 启动 wifi_task 和 minimp3_task。
2. wifi_task 等待 Wi-Fi 就绪，然后进入 dlan 的 ssdp_and_http_scan 循环。
3. dlan 收到 SOAP 控制命令：
   - SetAVTransportURI -> 保存 URI
   - Play/Pause/Stop -> 调用 minimp3 的静态控制接口
4. minimp3_task 主循环负责：
   - 建立 socket 并发起 GET 请求拉流
   - 解析 HTTP 响应头
   - 接收音频流字节
   - 解码 MP3 帧
   - 通过回调把 PCM 送入 IIS
5. wifi_task 侧再通过队列水位控制和 IIS buffer 管理来闭合播放节奏控制。

当前主要痛点：
- 单个模块承担了过多职责。
- 没有真实网络、真实解码器、真实 IIS 时很难做隔离测试。
- 重连、超时、非 MP3 数据识别等规则散落在大循环里，难以推理和维护。

---

## 3. 推荐的模块化拆分方案

建议采用 6 层架构，外加 2 个横切能力模块。

## 3.1 第 0 层：平台抽象层（PAL）

职责：
- 把 OSAL/LWIP 等平台相关细节封装到稳定接口后面。

模块建议：
- pal_time
- pal_thread
- pal_socket
- pal_log
- pal_memory

为什么先做这一层：
- 上层逻辑会更容易测试。
- 后续如果平台适配变化，不会把所有业务模块都带崩。

示例接口：
```cpp
struct ITimer {
    virtual uint64_t now_ms() = 0;
    virtual void sleep_ms(uint32_t ms) = 0;
    virtual ~ITimer() = default;
};

struct ISocket {
    virtual bool connect(const char* host, uint16_t port, uint32_t timeout_ms) = 0;
    virtual int send_all(const uint8_t* data, size_t len) = 0;
    virtual int recv_some(uint8_t* data, size_t cap, uint32_t timeout_ms) = 0;
    virtual void close() = 0;
    virtual ~ISocket() = default;
};
```

## 3.2 第 1 层：网络传输基础层

职责：
- 只处理纯网络协议和数据收发机制，不掺杂 DLNA 业务语义。

模块建议：
- net_tcp_client
- net_http_client
- net_http_parser
- net_ssdp_server
- net_event_subscriber_notify

结合你的诉求，TCP 一定要单独拆出来，最少拆成以下几个模块：
- `TcpClient`：负责 connect、send、recv、timeout、close、errno 映射。
- `HttpClient`：负责拼请求、读取响应头、暴露 body 流接口。
- `HttpStreamReader`：负责缓冲读取，可选支持 ICY metadata 过滤。

这一层的输出契约：
- 统一返回标准化错误枚举，例如 Timeout、Closed、NetworkDown、ParseError。
- 不允许直接调用解码器。
- 不允许直接改写 dlan 或播放状态。

## 3.3 第 2 层：DLNA/UPnP 控制面

职责：
- SSDP 发现处理。
- HTTP 控制端点路由。
- SOAP 动作解析与响应生成。
- SUBSCRIBE/UNSUBSCRIBE 以及 NOTIFY 生成。

模块建议：
- dmr_device_description
- dmr_avtransport_service
- dmr_renderingcontrol_service
- dmr_connection_manager_service
- dmr_eventing_manager

这一层最重要的规则：
- 控制面绝不能自己去做解码，也不能直接拉取音频流。
- 控制面只负责产生命令和事件，并把它们交给 playback_session_manager。

## 3.4 第 3 层：播放会话编排层

职责：
- 持有播放状态机。
- 把控制面事件映射成数据面的生命周期控制。

核心模块：
- playback_session_manager

最小状态机建议：
- IDLE
- PREPARING
- PLAYING
- PAUSED
- STOPPING
- ERROR

输入事件：
- SetUri(uri)
- Play
- Pause
- Stop
- NetworkLost
- StreamError

输出动作：
- 启动或停止 stream worker
- 发布 transport state 给 DLNA NOTIFY
- 对外暴露进度和错误信息

## 3.5 第 4 层：媒体数据面

职责：
- 从流中拉取字节。
- 解码帧。
- 做声道与采样率适配。
- 在带背压控制的前提下，把 PCM 推给输出端。

模块建议：
- media_stream_worker
- media_demux_probe（轻量级格式探测）
- audio_decoder_mp3（minimp3 适配器）
- pcm_pacer（基于队列水位的节奏控制）
- audio_sink_iis

这里有两个硬规则：
- 这一层绝不能出现 SOAP/SSDP 逻辑。
- 数据面应该以“一个 session 对应一个主循环”的方式运行，而不是全局散乱状态共享。

## 3.6 第 5 层：应用组装层

职责：
- 进行依赖注入和模块装配。
- 启动任务。
- 做健康检查和监控挂接。

模块建议：
- app_bootstrap
- app_task_registry

---

## 4. 建议的目录结构

建议在你的 `receiving end` 域下面整理成如下结构：

```text
receiving end/
  includes/
    core/
      error_code.hpp
      result.hpp
      event_bus.hpp
    pal/
      pal_time.hpp
      pal_thread.hpp
      pal_socket.hpp
      pal_log.hpp
    net/
      tcp_client.hpp
      http_client.hpp
      http_parser.hpp
      ssdp_server.hpp
      notify_client.hpp
    dmr/
      dmr_server.hpp
      avt_service.hpp
      rcs_service.hpp
      cm_service.hpp
      eventing_manager.hpp
    playback/
      session_manager.hpp
      playback_state.hpp
    media/
      stream_worker.hpp
      format_probe.hpp
      decoder_mp3.hpp
      pcm_pacer.hpp
      audio_sink_iis.hpp
  realize/
    pal/
      pal_time_osal.cpp
      pal_thread_osal.cpp
      pal_socket_lwip.cpp
      pal_log_osal.cpp
    net/
      tcp_client.cpp
      http_client.cpp
      http_parser.cpp
      ssdp_server.cpp
      notify_client.cpp
    dmr/
      dmr_server.cpp
      avt_service.cpp
      rcs_service.cpp
      cm_service.cpp
      eventing_manager.cpp
    playback/
      session_manager.cpp
    media/
      stream_worker.cpp
      format_probe.cpp
      decoder_mp3.cpp
      pcm_pacer.cpp
      audio_sink_iis.cpp
    app/
      app_bootstrap.cpp
```

迁移说明：
- 在重构前期，旧模块不要立刻删除。
- 可以先通过 adapter wrapper 方式把旧实现包进新接口里，保证逐步迁移时可运行、可回退。

---

## 5. 接口契约（最重要）

在正式移动代码之前，必须先把稳定接口定义好。

## 5.1 命令与事件契约

```cpp
enum class PlaybackCmdType {
    SetUri,
    Play,
    Pause,
    Stop,
};

struct PlaybackCommand {
    PlaybackCmdType type;
    char uri[512];
};

enum class PlaybackEventType {
    StateChanged,
    Error,
    Progress,
};

struct PlaybackEvent {
    PlaybackEventType type;
    int code;
    char text[128];
};
```

## 5.2 字节流读取契约

```cpp
class IByteStream {
public:
    virtual bool open(const char* url) = 0;
    virtual int read(uint8_t* out, int cap) = 0; // >0 表示读到字节，0 表示连接关闭，<0 表示错误
    virtual void close() = 0;
    virtual ~IByteStream() = default;
};
```

## 5.3 解码器契约

```cpp
struct AudioFrame {
    int16_t* pcm;
    int samples_per_channel;
    int channels;
    int sample_rate;
};

class IAudioDecoder {
public:
    virtual void reset() = 0;
    virtual int feed(const uint8_t* data, int len) = 0;
    virtual bool decode_one(AudioFrame& out, int& bytes_consumed) = 0;
    virtual ~IAudioDecoder() = default;
};
```

## 5.4 输出端契约

```cpp
class IAudioSink {
public:
    virtual bool start(int sample_rate, int channels) = 0;
    virtual bool write(const int16_t* pcm, uint32_t samples) = 0;
    virtual int queue_level() const = 0;
    virtual void stop() = 0;
    virtual ~IAudioSink() = default;
};
```

---

## 6. 各模块如何有机结合

必须采用单向依赖，不允许互相乱调。

依赖方向建议如下：

- app -> dmr, playback
- dmr -> playback（发命令）
- playback -> media, dmr_eventing（上报状态）
- media -> net, decoder, sink
- net -> pal
- 所有层 -> core（error/result/log）

明确禁止的行为：
- dmr 直接调用 decoder 或 sink。
- net 直接修改播放状态。
- media 直接解析 SOAP。

推荐的参考流转：
1. 手机发送 SetAVTransportURI 和 Play。
2. dmr 解析 SOAP 后生成 `PlaybackCommand`。
3. playback_session_manager 状态切到 PREPARING。
4. media_stream_worker 通过 http_client 打开 URL。
5. decoder_mp3 完成解码，pcm_pacer 根据 sink 队列水位进行节奏控制。
6. sink 把数据写入 IIS DMA。
7. playback 发布 `StateChanged(PLAYING)` 事件给 dmr eventing。
8. dmr 向订阅方发出 NOTIFY。

---

## 7. 线程模型建议

线程模型要尽量简单、确定、可控。

建议线程划分：
- control_thread：负责 SSDP + HTTP 控制服务（DMR）
- playback_thread：负责拉流 + 解码 + 输出
- 可选 monitor_thread：负责 watchdog 和统计监控

线程间通信方式：
- control_thread -> playback_thread 使用无锁环形队列或有界消息队列。
- playback 的状态和事件再通过另一条有界队列回传。

线程规则：
- 只有 playback_thread 可以碰解码器状态和 sink 写入。
- control_thread 不能阻塞在长时间的拉流操作上。
- 所有队列都必须是有界队列，并且定义溢出策略。

---

## 8. 错误模型与重连策略

建议统一错误枚举，不要各模块各自定义模糊错误语义。

```cpp
enum class MediaError {
    None,
    UrlInvalid,
    DnsFail,
    ConnectFail,
    HttpStatusBad,
    UnsupportedContent,
    StreamTimeout,
    DecodeStall,
    SinkBackpressure,
    Internal,
};
```

策略建议：
- 只有 playback session manager 可以决定是否重试。
- 重连采用指数退避并加轻微随机抖动：200ms -> 400ms -> 800ms，最大不超过 5s。
- 达到最大重试次数后自动停止并进入 ERROR 状态。
- 如果 URI 被更新，旧的重试流程要立即取消。

---

## 9. HTTP/TCP 的详细拆分方式（这是你最关心的部分）

如果想把边界拆干净，最少要拆成以下四块：

1) tcp_client
- connect(host, port, timeout)
- send_all(buffer)
- recv_some(buffer, timeout)
- set_timeouts
- close

2) http_client
- open_get(url, headers)
- read_headers()
- status_code()
- header_value(key)
- body_read_some()
- close

3) stream_filter
- 过滤 ICY metadata
- 预留 dechunk 能力（未来再做）
- 检查 content encoding

4) format_probe
- 识别明显不是 MP3 的签名
- 识别 ID3 与 MP3 sync

你当前 `minimp3.cpp` 里的逻辑，后续可以渐进式搬到第 2、3、4 项中，最后让 decode loop 只保留“解码”本身，不再承担网络和协议处理职责。

---

## 10. 迁移计划（渐进、安全）

## Phase 1：先定接口
- 先创建契约头文件，包括 playback command/event、stream、decoder、sink。
- 旧实现先不删，而是包在新接口后面。
- 这一阶段不追求行为变化，只追求边界稳定。

退出标准：
- 编译通过。
- 运行行为与当前版本一致。

## Phase 2：提取 TCP 与 HTTP
- 把 socket connect/send/recv、timeout 处理从 decoder 模块里挪出去。
- 实现 tcp_client + http_client。
- 把拉流链路中直接调用 lwip 的代码替换掉。

退出标准：
- 现有流仍然能播放。
- 重连仍然可用。

## Phase 3：拆出 Playback Session Manager
- 把 play/pause/stop/url 状态统一收进 session manager。
- 解码任务变成一个只接受命令的纯 worker。

退出标准：
- SOAP 的 Play/Pause/Stop 全部通过命令队列进入播放线程。

## Phase 4：重构数据面
- 创建 stream_worker、decoder_mp3 adapter、sink_iis adapter、pcm_pacer。
- 保留现有的队列水位策略，但把它集中进 pcm_pacer。

退出标准：
- 队列压力下不再出现明显“快进感”。
- 不再频繁出现 underrun 类日志。

## Phase 5：清理 DMR 服务
- 把 AVTransport、RenderingControl、ConnectionManager 的处理逻辑拆开。
- XML 生成逻辑按 service 独立隔离。

退出标准：
- 主流手机控制端能够稳定发现和控制。

## Phase 6：移除遗留静态耦合
- 去掉 dlan 与 minimp3 之间直接静态回调耦合。
- 改成显式接口和构造注入。

退出标准：
- 不再存在无关层之间共享的全局可变状态。

---

## 11. 测试策略

## 11.1 单元测试（能在 host 侧做的尽量先做）
- URL 解析
- HTTP header 解析
- content-type 与 transfer-encoding 识别
- format probe
- 状态机迁移
- pacing 策略

## 11.2 板级集成测试
- 手机 App 能发现 SSDP 广播设备
- SetURI + Play 成功
- Pause/Resume 连续性正常
- Stop 后立即静音并更新状态
- 弱网场景下重连行为符合预期

## 11.3 故障注入测试
- DNS 失败
- connect 超时
- 服务器中途断开连接
- 服务端返回非 MP3 body
- 响应头畸形

---

## 12. 可观测性与调试基线

建议增加结构化日志分类：
- NET
- DMR
- PLAYBACK
- DECODER
- SINK

建议每秒输出一条简洁统计：
- 接收字节数
- 解码帧数
- 输出 PCM 样本数
- sink 队列峰值
- 重试次数
- 当前状态

日志格式尽量可解析：
```text
[PLAYBACK] state=PLAYING url_hash=... q=12 recv_bps=... decode_fps=...
```

---

## 13. CMake 重构建议

你当前结构已经把 `includes` 和 `realize` 分开了，这是个不错的基础，下一步应该继续按模块域扩展。

实践建议：
- 给 pal/net/dmr/playback/media/app 分别增加独立 `CMakeLists.txt`。
- 在迁移期通过特性开关保留旧实现和新实现并存：

```cpp
#define DMR_REFACTOR_V2 1
```

开关策略：
- 开发分支默认打开 V2。
- 一旦发现阻塞问题，可以快速回退到旧路径。

---

## 14. 先拆什么最划算（优先级建议）

如果你想以最小风险获得最大收益，建议按这个顺序拆：
1. TCP client 抽象
2. HTTP 响应头解析与 stream reader
3. Playback session 状态机
4. Decoder adapter 与 sink adapter
5. DMR service handlers

原因：
- 前 3 项会立刻带来结构清晰度和稳定性提升。
- 后 2 项更多是建立在前面契约稳定之后的结构清理。

---

## 15. 当前文件与目标模块的映射关系

当前文件 -> 后续建议归属：

- `includes/http_utils/http_utils.cpp`
  - 只保留真正通用的工具函数。
  - URL 解析和 header 解析最终迁移到 `net/http_parser`。

- `includes/dlan/dlan.cpp`
  - 拆成 `dmr_server`、`avt_service`、`rcs_service`、`eventing_manager`、`ssdp_server`。

- `includes/miniMP3/minimp3.cpp`
  - 拆成 `media/stream_worker`、`media/decoder_mp3`、`net/http_client`、`media/format_probe`、`media/pcm_pacer`。

- `realize/wifi/wifi_task.cpp`
  - 最终只保留启动编排和依赖装配逻辑。

---

## 16. 新架构最小组装骨架示意

```cpp
class AppBootstrap {
public:
    void start() {
        dmr_server_.start();
        playback_manager_.start();
        dmr_server_.bind_command_sink(&playback_manager_);
        playback_manager_.bind_event_sink(&dmr_server_);
    }

private:
    TcpClient tcp_;
    HttpClient http_{tcp_};
    Mp3Decoder decoder_;
    IisSink sink_;
    PcmPacer pacer_{sink_};
    MediaStreamWorker worker_{http_, decoder_, pacer_};
    PlaybackSessionManager playback_manager_{worker_};
    DmrServer dmr_server_;
};
```

---

## 17. 验收清单

每一次重构迭代完成时，至少要满足以下条件：
- 设备可被发现，手机 App 能找到 renderer。
- SetURI/Play/Pause/Stop 都能正常工作。
- decoder 模块内部不再直接调用 lwip。
- stream/decoder 模块内部不再解析 SOAP。
- 队列背压逻辑只存在于一个专门模块里。
- 重连策略由 session manager 统一管理。
- 日志里的状态迁移清晰、稳定、可预测。

---

## 18. 最后的实践建议

- 不要一次性全量重写，必须保留旧链路并按模块逐步切换。
- 先锁定接口，再移动代码。
- 每做一次拆分，都要让原始大文件的职责数明显减少。
- 状态机要显式、要小，避免隐藏的静态状态四处扩散。

如果你愿意，下一步我可以继续为你补一版 V1 落地计划，直接细化到“要创建哪些头文件和 cpp 文件、每个文件先写什么骨架接口”。