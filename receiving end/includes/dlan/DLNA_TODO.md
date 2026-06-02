# DLNA 实现现状与待办事项

> 代码路径：`main/receiving end/includes/dlan/dlan.cpp` & `dlan.hpp`
> 分析日期：2026-05-07

---

## 一、已完成的功能 ✅

| 功能 | 说明 |
|------|------|
| SSDP 发现响应 | 正确响应 `ssdp:all`、`rootdevice`、`MediaRenderer:1`、`AVTransport:1`、`RenderingControl:1`、`ConnectionManager:1` 的 M-SEARCH |
| 设备描述 XML | `/description.xml` 完整返回设备信息和三个服务声明 |
| SCPD XML 响应 | `/AVTransport.xml`、`/RenderingControl.xml`、`/ConnectionManager.xml` 均有返回 |
| SUBSCRIBE / UNSUBSCRIBE | 事件订阅/取消正常处理，保存 SID 和 Callback URL |
| AVTransport 事件通知 | `notify_avtransport_state()` 实现，Play/Pause/Stop 后主动通知控制端 |
| RenderingControl 事件通知 | `notify_renderingcontrol_state()` 实现（但数据是硬编码的，见下文） |
| `SetAVTransportURI` | 解析 URI 和 metadata，调用 `media_set_uri_handler_func` 回调，并上报 STOPPED |
| `Play` | 调用 `media_play_handler_func` 回调，状态依次变为 TRANSITIONING → PLAYING/STOPPED |
| `Stop` | 调用 `media_stop_handler_func` 回调，状态变为 STOPPED |
| `GetTransportInfo` | 返回真实的 `g_transport_state` |
| `GetTransportSettings` | 返回固定值 `NORMAL`（正确） |
| `GetDeviceCapabilities` | 返回固定值（正确） |
| `GetCurrentTransportActions` | 根据当前状态动态返回可用动作（Play/Pause/Stop） |
| `GetMediaInfo` | 基本结构正确，URI 有值时返回 |
| `GetPositionInfo` | 播放时长计时（jiffies）正常工作，返回已播放的 `RelTime`/`AbsTime` |
| ConnectionManager 三个动作 | `GetProtocolInfo` / `GetCurrentConnectionIDs` / `GetCurrentConnectionInfo` 均正常 |
| SOAP Fault 响应 | 非法 MIME 类型时返回 `714 Illegal MIME-Type` |

---

## 二、存在的问题与未完成功能 ❌

### 2.1 Pause（暂停）—— 有协议外壳，无实际能力

**现状：**
```cpp
// wifi_task.cpp
dlan_.register_media_pause_handler(minimp3::stop_playback); // ← Pause 和 Stop 注册的是同一个函数
dlan_.register_media_stop_handler(minimp3::stop_playback);
```

**问题：**
- `Pause` 和 `Stop` 实际行为完全相同，都是**彻底停止播放**并清空 URL。
- 当控制端再次发 `Play` 时，`minimp3` 没有"续播"能力，会**从头重新拉流**，而非从断点继续。
- `media_status.is_paused` 字段在 `dlan.hpp` 中定义，但在整个 `dlan.cpp` 中从未被读写过。

**需要做的工作：**
1. 在 `minimp3` 中增加真正的暂停状态（暂停 DMA/IIS 输出，保留 TCP 连接和缓冲区）；
2. 区分 Pause 和 Stop 的回调，注册不同的函数；
3. `Play` 命令需区分：从 `PAUSED_PLAYBACK` 状态恢复 vs 从 `STOPPED` 状态全新启动。

---

### 2.2 音量控制（SetVolume / GetVolume / SetMute / GetMute）—— 完全是空壳

**现状：**
```cpp
// SetVolume：只回复成功，不做任何事
send_http_soap_response(client_sock, set_volume_response_body);
// GetVolume：硬编码返回 50
"<CurrentVolume>50</CurrentVolume>"
// SetMute：只回复成功，不做任何事
// GetMute：硬编码返回 0
"<CurrentMute>0</CurrentMute>"
```

**问题：**
- 控制端调整音量后，设备音量**不会有任何变化**。
- `dlan.hpp` 中的 `media_status.volume` 和 `media_status.mute` 字段从未被读写。
- SUBSCRIBE 回调时 `notify_renderingcontrol_state(50, false)` 是硬编码，无法反映真实状态。
- `dlan` 类没有 `volume_handler` 类型的回调，无法通知外部模块。

**需要做的工作：**
1. 在 `dlan.hpp` 中新增 `using volume_set_handler = void (*)(uint8_t volume)` 和 `using mute_set_handler = void (*)(bool mute)` 回调类型；
2. `SetVolume` 中解析 SOAP body 中的 `<DesiredVolume>` 字段，调用回调并更新 `media_status.volume`；
3. `SetMute` 中解析 `<DesiredMute>` 字段，调用回调并更新 `media_status.mute`；
4. `GetVolume` / `GetMute` 返回 `media_status.volume` / `media_status.mute` 的真实值；
5. 接入 CS43131 DAC 的音量寄存器（PCM Volume A/B 寄存器，在 cs43131_pdf_init_sequence.md 中已有记录）；
6. 订阅回复时用真实状态值，而非硬编码 `(50, false)`。

---

### 2.3 下一首 / 上一首 —— DLNA 协议层面说明

**DLNA DMR 本身不提供 Next/Previous Track 的 SOAP 动作**，这由控制端（DMC，手机 App）负责：控制端维护播放列表，发送完当前歌曲后主动推送下一首的 `SetAVTransportURI` + `Play`。

**但目前有一个相关缺陷：`SetNextAVTransportURI` 是空操作**

```cpp
} else if (soap_action_has(soap_action_value.data(), "SetNextAVTransportURI")) {
    osal_printk("http收到 SetNextAVTransportURI 命令，按空操作兼容处理\n");
    // 直接返回成功，next URI 被丢弃
}
```

**问题：**
- 部分控制端（如 BubbleUPnP、Kazoo）会提前用 `SetNextAVTransportURI` 预加载下一曲，期望当前曲播放完毕后**自动无缝切换**。
- 目前这个 URI 被直接丢弃，自动切歌功能无法工作。

**需要做的工作：**
1. 用一个静态变量 `g_next_uri` 保存 next URI；
2. 当 minimp3 流自然结束时（需要增加"播放结束"回调），自动将 `g_next_uri` 设为当前 URI 并触发播放。

---

### 2.4 Seek（进度跳转）—— 完全未实现

**现状：** `Seek` 动作未在 AVTransport SCPD XML 的 `<actionList>` 中声明，因此控制端通常不会发送此命令，但部分 App 会尝试。

**问题：**
- 即使在 XML 中声明，HTTP 直播流的 Seek 需要发送带 `Range` 头的新请求，目前 minimp3 的 HTTP 拉流实现不支持。

**需要做的工作（若需要）：**
1. 在 AVTransport.xml 中增加 `<action><name>Seek</name></action>` 声明；
2. 在 SOAP 处理中解析 `<Unit>` 和 `<Target>` 字段；
3. 修改 minimp3 的 HTTP 拉流逻辑，支持 `Range: bytes=xxx` 重新建立连接（适用于非直播的点播文件流）。

---

### 2.5 曲目时长（TrackDuration / MediaDuration）—— 硬编码为零

**现状：**
```cpp
// GetMediaInfo
"<MediaDuration>00:00:00</MediaDuration>"
// GetPositionInfo
"<TrackDuration>%s</TrackDuration>", "00:00:00"  // 始终是 00:00:00
```

**问题：**
- 控制端（手机）进度条无法正常显示总时长，进度百分比始终为 0。
- 网络流媒体时长获取有难度（直播流没有时长；点播文件可尝试从 HTTP `Content-Length` 和平均比特率估算）。

**需要做的工作（若需要）：**
- 从 HTTP 响应头中提取 `Content-Length`，结合 minimp3 解码到的平均比特率估算总时长；
- 或从 ID3v2 tag 解析 TLEN 帧。

---

### 2.6 SetPlayMode —— 未实现

**现状：** AVTransport SCPD XML 中未声明，`GetTransportSettings` 硬编码返回 `NORMAL`。

**问题：**
- 控制端无法切换随机/单曲循环/全部循环等播放模式。

**需要做的工作（若需要）：**
1. 在 AVTransport.xml 中增加 `<action><name>SetPlayMode</name></action>` 声明；
2. 保存播放模式状态，`GetTransportSettings` 返回真实值。

---

### 2.7 播放自然结束后状态未同步

**现状：** minimp3 的 `stream_mp3_to_iis()` 在流结束/断网后会停止，但 `g_transport_state` 不会自动从 `PLAYING` 变回 `STOPPED`，也不会向控制端发送 NOTIFY。

**问题：**
- 控制端 UI 可能一直显示"正在播放"，与实际状态不符。
- 控制端因此无法自动触发下一曲。

**需要做的工作：**
1. 在 `minimp3` 中增加"播放结束"回调类型 `using playback_end_handler = void (*)()`；
2. 流结束时调用该回调；
3. 在 `wifi_task.cpp` 中注册该回调，内部调用 `update_transport_state("STOPPED", true)`。

---

### 2.8 SSDP 主动通告（ssdp:alive / ssdp:byebye）—— 未实现

**现状：** 只响应 M-SEARCH，不主动发送 `ssdp:alive` 公告。

**问题：**
- 部分控制端（尤其是 PC 端软件）不会主动发 M-SEARCH，而是等待设备广播 alive，导致设备无法被发现。
- 设备关机时没有 `ssdp:byebye`，控制端需要等待缓存超时（1800 秒）才会把设备移除。

**需要做的工作（可选）：**
1. 启动时向 `239.255.255.250:1900` 发送三次 `ssdp:alive` 通知；
2. 每隔 `ssdp_timeout / 2` 秒重新发送一次 alive；
3. 关闭时发送 `ssdp:byebye`。

---

### 2.9 `now_media_status` 对象未被使用

**现状：** `dlan.hpp` 中定义了：
```cpp
class media_status {
    bool has_medio = false;
    bool is_playing = false;
    bool is_paused = false;
    uint32_t duration_seconds = 0;
    uint32_t position_seconds = 0;
    uint8_t volume = 50;
    bool mute = false;
} now_media_status;
```
但 `dlan.cpp` 中的任何地方都没有读写过 `now_media_status` 的任何字段。

**需要做的工作：**
- 将上述 2.2 的音量/静音状态、2.5 的时长状态存入 `now_media_status`，使其真正发挥作用；
- 或将其删除以减少混淆。

---

## 三、总结优先级建议

| 优先级 | 功能 | 影响程度 |
|--------|------|----------|
| 🔴 高 | Pause 真正暂停（不同于 Stop） | 用户体验核心功能 |
| 🔴 高 | 播放结束后状态自动同步为 STOPPED | 控制端 UI 与实际状态一致 |
| 🟡 中 | 音量控制接入 CS43131 硬件 | 手机端调音量无效 |
| 🟡 中 | SetNextAVTransportURI 保存 next URI，实现自动切歌 | 自动播放列表功能 |
| 🟢 低 | SSDP alive/byebye 主动通告 | 兼容性优化 |
| 🟢 低 | TrackDuration 时长估算 | UI 进度条显示 |
| 🟢 低 | SetPlayMode 播放模式 | 可选功能 |
| 🟢 低 | Seek 进度跳转 | 仅对点播流有意义，直播流无需 |
