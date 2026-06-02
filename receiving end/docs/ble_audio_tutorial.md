# WS63 A2DP Sink 蓝牙音频接收完全教程

> **目标读者：** 写过嵌入式 C/C++，没有写过蓝牙，首次接触经典蓝牙（BR/EDR）音频协议。
>
> **本文目标：** 让 WS63 作为一个蓝牙音箱（A2DP Sink），手机可直接在蓝牙设置里搜到并连接，不需要手机安装任何 App。
>
> **代码风格：** 与项目内已有的 `sle.hpp / sle.cpp` 保持一致，静态 C++ 封装类。

---

## 第零章：在开始之前——你必须了解的根本区别

### 0.1 蓝牙不是星闪，你不是主导者

写星闪（SLE）的时候，整个流程是这样的：
```
你 ──广播──► 对端设备发起连接 ──► 你设定服务/属性 ──► 对端向你写数据
```
**你的 WS63 是主动方**，你决定广播什么，你决定开服务，你决定 MTU。数据什么格式、什么时候来，你说了算。

经典蓝牙 A2DP 是这样的：
```
手机 ──发现你──► 手机发起配对 ──► 手机发起 A2DP 连接 ──► 手机选 Codec ──► 手机流式传输 SBC 数据
```
**手机是主导方（Source）**，你的 WS63 是被动方（**Sink，音箱角色**）。  
你能做的是：
- 告诉手机"我支持 SBC / AAC"（宣告能力）
- 同意或拒绝手机提出的配对请求
- 同意或拒绝手机选择的 Codec 参数

> **核心心法：** A2DP Sink 的工作就是**等待**——等手机来连、等手机来发数据、等手机来暂停。你的代码只负责把解码后的 PCM 送到 IIS，其他都是响应手机的动作。

---

### 0.2 经典蓝牙（BR/EDR）与 BLE 的根本区别

| 对比项 | 经典蓝牙 BR/EDR | BLE（低功耗蓝牙） |
|--------|---------------|-----------------|
| 用途 | **高速音频/数据流**（A2DP 音频、SPP 串口） | 低功耗传感、短消息通知 |
| 带宽 | 最高约 3 Mbps（A2DP 实际约 300kbps） | 最高约 2 Mbps（实际 ATT 层约 150kbps） |
| 协议栈使能 | `enable_bt_stack()` | `enable_ble()` |
| 发现机制 | 广播 + 扫描（Inquiry/Page） | 广播包（ADV） |
| 音频支持 | 原生 A2DP Profile，SBC 硬件解码 | 无标准音频 Profile |
| 手机连接方式 | **直接从蓝牙设置里连接，就像连蓝牙耳机** | 需要专用 App 通过 GATT 写数据 |
 
**结论：** 要让手机像连蓝牙音箱一样直连，必须用**经典蓝牙 BR/EDR + A2DP**，不是 BLE。

---

### 0.3 WS63 的蓝牙协议栈架构

```
[ 手机（A2DP Source） ]
         │
    SBC / AAC 编码数据
         │
[ libbth_sdk.a（海思内置蓝牙协议栈）]
         │  ← 你的代码通过 API 与这里交互
    协议处理：L2CAP → AVDTP → A2DP
    解码：SBC 解码在这里完成 ──────────────────┐
         │                                     │
[ bt_audio_hal_interface.h API ]         解码后 PCM
         │                                     │
[ bt_attach_audio_port() ]                     │
         │ share_mem_id                         │
         ▼                                     ▼
   [ 音频硬件路由 ]          ─ 或 ─    [ 共享内存缓冲 ]
         │                                     │
    I2S 硬件直输出                    你读取 → iis::data_write()
```

> **重要说明：** `bt_attach_audio_port()` 中的 `share_mem_id` 是 WS63 平台的音频路由 ID。根据海思嵌入式平台的惯例，这个 ID 很可能对应硬件 I2S 输出路径——即 BT 协议栈解码完直接写入 I2S DMA 缓冲，**绕过你的 `iis::data_write()`**。实测时请以 `share_mem_id = 0` 作为默认值先尝试。

---

## 第一章：A2DP 协议栈结构与你的职责

### 1.1 A2DP 连接建立完整流程

```
手机                                    WS63 (你)
  │                                        │
  │── Inquiry（搜索设备） ────────────────► │  你必须处于可发现状态
  │◄─ Inquiry Response（我是 WS63-Speaker）─│  GAP: GAP_SCAN_MODE_CONNECTABLE_GENERAL_DISCOVERABLE
  │                                        │
  │── Connection Request ─────────────────►│
  │◄─ Connection Response ─────────────────│  ACL 链路建立
  │                                        │
  │── Pairing Request ────────────────────►│  第一次连接需要配对
  │◄─ Confirm Pairing ─────────────────────│  你需要自动确认（或用户确认）
  │                                        │
  │── A2DP AVDTP Discover ────────────────►│  "你支持哪些 Codec?"
  │◄─ SBC + AAC Capabilities ──────────────│  你声明能力（libbth_sdk.a 自动处理）
  │                                        │
  │── Set Configuration (选 SBC/AAC) ──────►│  手机决定用哪个 Codec
  │◄─ Accept ──────────────────────────────│
  │                                        │
  │── Open Stream ────────────────────────►│  → BT_AUDIO_A2DP_STREAM_OPENED 事件
  │◄─ Accept ──────────────────────────────│  你在这里调用 bt_attach_audio_port()
  │                                        │
  │── Start Stream（用户按播放）───────────►│  → BT_AUDIO_A2DP_STREAM_STRAT 事件
  │                                        │  你在这里调用 bt_start_audio_stream()
  │─ SBC 音频数据 ── SBC 音频数据 ─────────►│  BT 协议栈持续接收并解码
  │                                        │  PCM → 音频硬件 / 共享内存
  │── Suspend（用户按暂停）────────────────►│  → BT_AUDIO_A2DP_STREAM_SUSPENDED 事件
  │                                        │  你调用 bt_pause_audio_stream()
  │── Close Stream ────────────────────────►│  → BT_AUDIO_A2DP_STREAM_CLOSED 事件
  │                                        │  你调用 bt_stop_audio_stream()
```

> **关键认知：** 你的代码是**被动响应**的。上面的每一个 `BT_AUDIO_A2DP_*` 事件，都通过 `bt_register_audio_listener()` 注册的回调函数通知你。你在回调里响应就行了。

---

### 1.2 除了 A2DP，还需要 AVRCP Target

A2DP 只负责传输音频数据。手机的播放/暂停/下一首按钮是通过另一个协议 **AVRCP（音频/视频遥控协议）** 传递的。

```
手机上按"暂停" ──AVRCP PassThrough(PAUSE)──► 你的 AVRCP Target 回调
                                                  │
                                       你调用 bt_pause_audio_stream()
                                       同时通知 iis::data_clear()
```

在你的设备里，你是 **AVRCP Target（TG，目标设备）**，手机是 **AVRCP Controller（CT，控制器）**。
- TG = 音箱/耳机角色，响应控制命令
- CT = 手机/遥控器角色，发出控制命令

---

## 第二章：编解码器支持全面分析

### 2.1 强制支持：SBC（Sub-Band Coding）

SBC 是蓝牙音频的**强制基础编解码器**，每一台支持 A2DP 的设备都必须支持。WS63 的 `libbth_sdk.a` 内部实现了 SBC 解码器。

SDK 中的证据（`bt_audio_hal_interface.h` 第107行注释）：
```c
BT_AUDIO_A2DP_SBC_SF_44100 = 0x20, /* SRC must support 44.1 or 48, SNK must support both */
```
`SNK must support both` = 明确说明 Sink（音箱）必须同时支持 44100Hz 和 48000Hz。

SBC 参数范围（对应我们的 44100Hz 立体声场景）：
```c
// bt_audio_hal_interface.h 中定义的 SBC 能力结构体
typedef struct {
    td_u32 sample_frequency;  // 44100 或 48000（Hz）
    td_u8  chnl_mode;         // JOINT_STEREO（联合立体声，音质最好）
    td_u8  block_length;      // 16（建议值，越大压缩效率越高）
    td_u8  subband;           // 8（建议值）
    td_u8  alloc_method;      // LOUDNESS（响度分配，大多数耳机只支持这个）
    td_u8  min_bitpool;       // 2（最低码率）
    td_u8  max_bitpool;       // 53（对应 ~320kbps @44100Hz 立体声）
} bt_a2dp_sbc_codec_caps;
```

### 2.2 可选支持：AAC（Advanced Audio Coding）

AAC 在 `bt_audio_hal_interface.h` 中有完整的能力结构体定义（`bt_a2dp_mpeg24_codec_caps`），说明 SDK **支持 AAC 解码**。

AAC 相比 SBC 的优势：相同码率下音质更好，iOS 设备默认优先使用 AAC。

```c
// AAC 能力结构体
typedef struct {
    td_u32 sample_frequency;  // 44100 / 48000 Hz
    td_u32 bit_rate;          // 比特率（bps），AAC 通常 128000~256000
    td_u8  channels;          // 1=单声道, 2=立体声
    td_u8  object_type;       // MPEG4_LC（最常用的 AAC 子类型）
    td_u8  vbr;               // 1=可变码率, 0=固定码率
} bt_a2dp_mpeg24_codec_caps;
```

### 2.3 高级编解码器：LHDC / LDAC / aptX / L2HC 的真实实现路径

#### 2.3.1 `a2dp_codec_type_t` 是平台级枚举，不是单方向的

在 `bts_a2dp_source.h` 的 `a2dp_codec_type_t` 中：
```c
typedef enum {
    A2DP_CODEC_TYPE_SBC    = 0x00,
    A2DP_CODEC_TYPE_AAC,
    A2DP_CODEC_TYPE_APTX,        // 高通 aptX
    A2DP_CODEC_TYPE_APTX_HD,     // 高通 aptX HD
    A2DP_CODEC_TYPE_LDAC,        // 索尼 LDAC
    A2DP_CODEC_TYPE_LHDC,        // Savitech LHDC（Hi-Res Audio，在中国安卓设备中极为普遍）
    A2DP_CODEC_TYPE_LC3,         // 蓝牙官方 LE Audio 编码器
    A2DP_CODEC_TYPE_L2HC,        // 华为自研 L2HC（Hi-Res，高保真）
} a2dp_codec_type_t;
```

该文件名为 `bts_a2dp_source.h`，但这个枚举是**平台级**的——它代表 WS63 控制器固件支持的全部编解码器类型。它出现在 Source 头文件里，是因为只有 Source（发送端）需要在用户层**主动选择**使用哪种编解码器；而 Sink（接收端）是被动响应的，用户层不需要主动选择，所以没有对应的 Sink 选择 API，但解码能力是对称实现的。

#### 2.3.2 `BT_AUDIO_CODEC_UNKNOWN = 0xFF` 其实是 Vendor Specific 通道

HAL 层定义的四个编解码器码：
```c
#define BT_AUDIO_CODEC_SBC      0x00  // A2DP 标准 SBC
#define BT_AUDIO_CODEC_MPEG12   0x01  // A2DP 标准 MPEG-1/2 (MP3)
#define BT_AUDIO_CODEC_MPEG24   0x02  // A2DP 标准 MPEG-2/4 (AAC)
#define BT_AUDIO_CODEC_UNKNOWN  0xFF  // ← 这不是"未知"，是 A2DP 标准的 Vendor Specific！
```

这四个值**完全对应 A2DP 协议规范中的 Codec ID**：
- `0x00` = SBC（强制）
- `0x01` = MPEG-1,2 Audio
- `0x02` = MPEG-2,4 AAC
- `0xFF` = **Vendor Specific**（厂商自定义）

**LHDC、LDAC、aptX、aptX-HD、L2HC 在 A2DP 协议中全部使用 `0xFF` 这个 Codec ID！** 这正是 `BT_AUDIO_CODEC_UNKNOWN = 0xFF` 的真实含义。

#### 2.3.3 `codec_caps[16]` 承载厂商编解码器的完整参数

当 `codec_type = 0xFF` 时，`codec_caps[16]` 字节的内容按 A2DP Vendor Specific 格式解析：
```
codec_caps[0..3] = Company ID（4字节厂商 ID）
  LHDC: 0x00, 0x00, 0x2D, 0x01  (Savitech)
  LDAC: 0x2D, 0x01, 0x00, 0x00  (Sony)
  aptX: 0x4F, 0x00, 0x00, 0x00  (Qualcomm CSR)
  L2HC: 海思内部定义

codec_caps[4..15] = 厂商自定义能力参数（采样率、位深、声道等）
```

HAL 不需要为每个厂商编解码器单独定义结构体，因为它们全都走 `0xFF` 这一条通道。

#### 2.3.4 编解码器实现在控制器固件中，不在用户空间库里

通过对所有蓝牙 `.a` 库（包括 `libbt_host.a` 1.1MB、`libbgtp.a` 1.4MB）的二进制字符串扫描，所有库中都没有找到 "sbc"、"aac"、"lhdc"、"ldac" 等可打印字符串。这不代表不支持，而是说明：

**编解码器实现在 WS63 控制器的 ROM 固件中运行，不是用户空间代码。** 这是 IoT BT SoC 的标准架构——控制器固件（BGTP，蓝牙协议栈固件）负责全部编解码，`libbth_sdk.a` 等库只是与固件通信的 IPC 封装。这也解释了为什么 `libbt_host.a` 中找到了 `btsdk_set_audio_shm_id` 函数——解码后的 PCM 通过共享内存从控制器固件传递到应用侧。

#### 2.3.5 各编解码器的实际支持判断

| 编解码器 | 协议 ID | 支持可能性 | 依据 |
|---------|---------|----------|------|
| **SBC** | 0x00 | ✅ 必须支持 | A2DP 标准强制要求 |
| **AAC** | 0x02 | ✅ 已确认 | HAL 有完整 AAC 能力结构体 |
| **L2HC** | 0xFF | ✅ 极大概率 | 华为自研，WS63 = 海思芯片，自家编解码器必然内置 |
| **LHDC** | 0xFF | ✅ 很可能 | 目标市场（中国高端安卓）普遍支持，枚举中显式列出 |
| **LC3** | 0xFF | ✅ 可能 | 蓝牙 SIG 标准，枚举中显式列出 |
| **aptX/aptX-HD** | 0xFF | ⚠️ 不确定 | 需高通许可，视授权合同而定 |
| **LDAC** | 0xFF | ⚠️ 不确定 | 需索尼许可，视授权合同而定 |

> **结论：** 固件内置 SBC + AAC 可以确认。L2HC（华为自研）和 LHDC（中国市场主流）极大概率被 WS63 固件支持。aptX 和 LDAC 依赖第三方许可，不确定。
>
> **实践建议：** SBC 和 AAC 优先跑通（覆盖 100% 设备）。L2HC/LHDC 的协商由控制器固件自动处理——如果手机和 WS63 固件都支持，协商会自动发生，你的代码无需任何修改，`codec_type = 0xFF` 时读 `codec_caps` 的 Company ID 即可判断正在使用哪种高级编解码器。

---

## 第三章：相关头文件速查

```cpp
// 经典蓝牙 GAP（通用访问协议）
#include "bts_br_gap.h"
// 提供：enable_bt_stack(), disable_bt_stack()
// 提供：bluetooth_set_local_name(), bluetooth_set_local_addr()
// 提供：gap_br_set_bt_scan_mode()  ← 设置设备可被手机发现
// 提供：gap_register_callbacks()   ← 注册配对/连接回调

// A2DP 音频 HAL 接口（最重要的文件）
#include "bt_audio_hal_interface.h"
// 提供：bt_register_audio_listener()   ← 监听 A2DP 流事件
// 提供：bt_attach_audio_port()         ← 绑定音频输出端口
// 提供：bt_start_audio_stream()        ← 启动音频流（开始解码）
// 提供：bt_pause_audio_stream()        ← 暂停
// 提供：bt_stop_audio_stream()         ← 停止
// 提供：bt_set_audio_parameter()       ← 设置 Codec 参数（可指定首选 AAC）
// 提供：bt_get_audio_parameter()       ← 获取当前 Codec 参数

// AVRCP Target（响应手机播放/暂停/音量控制）
#include "bts_avrcp_target.h"
// 提供：avrcp_tg_register_callbacks()
// 提供：avrcp_tg_notify_playback_status_changed()  ← 主动通知手机播放状态
// 提供：avrcp_tg_notify_volume_changed()           ← 通知手机本机音量

// AVRCP 媒体控制回调（pass-through 按键事件）
#include "bt_audio.h"
// 提供：bt_avrcp_tg_register_audio_cbk()  ← 接收手机 play/pause/next 按键
```

---

## 第四章：CoD（设备类型码）——让手机认出你是音箱

### 4.1 什么是 CoD

CoD（Class of Device）是经典蓝牙的设备分类码，手机扫描到你时，通过 CoD 判断这是什么设备，然后在蓝牙列表里显示对应图标（耳机、音箱、键盘等）。

CoD 是一个 24 位数字，格式：
```
[23:16] Major Service Class  | [12:8] Major Device Class | [7:2] Minor Device Class
  音频类 = SERVICE_AUDIO(0x200000)  |  音频设备 = 0x0400   |  扬声器 = 0x04
```

对于蓝牙音箱，CoD 值应该是：
```c
// Major Service: Audio(0x200000) + Render(0x040000) = 0x240000
// Major Device: Audio/Video = 0x0400
// Minor Device: Loudspeaker = 0x04 (左移2位 = 0x10)
// 最终 CoD = 0x240000 | 0x0400 | 0x10 = 0x240410
#define BLUETOOTH_SPEAKER_COD  0x240410
```

> **注意：** `bts_br_gap.h` 中没有直接设置本地 CoD 的 API，CoD 通常在蓝牙协议栈初始化阶段通过配置文件设定，或者由已注册的 Profile 自动推断。`libbth_sdk.a` 会根据已注册的 A2DP Sink Profile 自动设置正确的 CoD。

---

## 第五章：完整代码实现

### 5.1 头文件 `bt.hpp`

```cpp
#pragma once

// ============================================================
// 经典蓝牙 (BR/EDR) A2DP Sink 接收端封装
// 目标：让手机像连蓝牙音箱一样连接 WS63，无需手机安装任何 App
//
// 所用协议：
//   GAP (BR/EDR)  - 设备发现、配对
//   A2DP Sink     - 音频流接收与解码（SBC/AAC）
//   AVRCP Target  - 响应手机播放/暂停/音量控制
// ============================================================

extern "C" {
#include "bts_br_gap.h"               // GAP: 使能蓝牙、设置可发现、配对回调
#include "bt_audio_hal_interface.h"   // A2DP HAL: 流事件、音频端口绑定
#include "bts_avrcp_target.h"         // AVRCP TG: 响应手机播放控制
#include "bt_audio.h"                 // AVRCP 媒体按键回调
#include "errcode.h"
#include "soc_osal.h"
}

class bt {
public:
    // ── 构造函数 ──────────────────────────────────────────
    // 注册所有回调后调用 enable_bt_stack()
    // 注意：bt_stack 使能是异步的，真正的初始化在 bt_stack_state_cb 里完成
    bt();

    // ── 外部数据注入接口（与 sle 保持相同风格）──────────────
    // 注意：由于 A2DP 音频由协议栈内部路由到硬件，这两个接口
    //       在直接硬件输出模式下可能不被调用；
    //       在共享内存模式下，data_process 用于接收 PCM 数据
    using data_process_t = void (*)(const int16_t *data, uint32_t length);
    using data_clear_t   = void (*)();
    static void set_data_process_function(data_process_t callback);
    static void set_data_clear_function(data_clear_t callback);

private:
    // ── 初始化函数（在 BT stack 使能回调中按序调用）────────
    static void setup_local_device();      // 设置设备名、地址
    static void setup_scan_mode();         // 设置为可发现+可连接
    static void setup_audio_listener();    // 注册 A2DP 音频流监听

    // ── GAP 回调函数组 ────────────────────────────────────
    // BT stack 状态变化（等同于 SLE 的 sle_enable_callback）
    static void bt_stack_state_cb(const int transport, const int status);
    // ACL 连接状态变化（手机连上/断开）
    static void acl_state_changed_cb(const bd_addr_t *bd_addr, gap_acl_state_t state, unsigned int reason);
    // 配对请求（手机想和你配对，需要你同意）
    static void pair_requested_cb(const bd_addr_t *bd_addr);
    // 配对确认（手机显示配对码，需要你确认）
    static void pair_confirmed_cb(const bd_addr_t *bd_addr, int req_type, int number);
    // 配对状态变化（配对完成/失败）
    static void pair_status_changed_cb(const bd_addr_t *bd_addr, int status);

    // ── A2DP HAL 音频流事件回调 ────────────────────────────
    // 这是核心回调，处理 Stream 的整个生命周期
    static void audio_event_cb(bt_audio_event_type type,
                                const td_void *data,
                                int32_t size,
                                td_void *context);

    // ── AVRCP 媒体按键事件（手机按播放/暂停/下一首）─────────
    static void avrcp_passthrough_cb(td_u32 key_operation, td_u32 key_value);

    // ── 数据回调函数指针 ──────────────────────────────────
    static data_process_t data_process;
    static data_clear_t   data_clear;

    // ── 当前流句柄（在 STREAM_OPENED 事件中获得）───────────
    // 后续 start/pause/stop/attach 都要用这个 handle
    static td_pvoid current_stream_hdl;

    // ── 连接状态 ──────────────────────────────────────────
    static bool is_connected;

    // ── 设备配置常量 ──────────────────────────────────────
    // 手机蓝牙搜到的设备名
    static constexpr const char *device_name = "WS63-Speaker";
    // 蓝牙地址（6字节，LSB在前）
    static constexpr uint8_t local_addr[6] = {0xA1, 0x11, 0x00, 0x03, 0x26, 0x20};

    // 音频端口 ID：共享内存 ID，0 是默认值（对应硬件 I2S 输出路径）
    // 如果 0 不工作，可能需要查阅海思 BT 音频驱动文档获取正确 ID
    static constexpr td_u32 audio_port_id = 0;
};
```

---

### 5.2 实现文件 `bt.cpp`

```cpp
#include "bt.hpp"
// 如果需要手动输出到 IIS（共享内存模式），取消下面的注释
// #include "../iis/iis.hpp"

// ============================================================
// 静态成员变量定义
// ============================================================
bt::data_process_t bt::data_process = nullptr;
bt::data_clear_t   bt::data_clear   = nullptr;
td_pvoid           bt::current_stream_hdl = nullptr;
bool               bt::is_connected = false;

// ============================================================
// 外部接口：注入数据回调
// ============================================================
void bt::set_data_process_function(data_process_t callback) {
    data_process = callback;
}
void bt::set_data_clear_function(data_clear_t callback) {
    data_clear = callback;
}

// ============================================================
// 构造函数：注册所有回调，然后启动蓝牙协议栈
//
// 注意：enable_bt_stack() 是异步的！
//       真正的初始化逻辑写在 bt_stack_state_cb 里，
//       等到 status == BT_STACK_STATE_TURN_ON 时才执行。
//       这和 SLE 里 sle_enable_callback 的设计完全一样。
// ============================================================
bt::bt() {
    // ── 第一步：准备 GAP 回调结构体 ──────────────────────
    //
    // gap_call_backs_t 是一个结构体，里面全是函数指针
    // 只需要填写你关心的回调，其他留 nullptr
    static gap_call_backs_t gap_cbs = {0};
    gap_cbs.state_change_callback        = bt_stack_state_cb;      // BT 开关状态
    gap_cbs.acl_state_changed_callbak    = acl_state_changed_cb;   // 连接状态
    gap_cbs.pair_requested_callback      = pair_requested_cb;      // 收到配对请求
    gap_cbs.pair_confiremed_callback     = pair_confirmed_cb;      // 配对码确认
    gap_cbs.pair_status_changed_callback = pair_status_changed_cb; // 配对完成

    int ret = gap_register_callbacks(&gap_cbs);
    if (ret != 0) {
        // gap_register_callbacks 失败，通常是参数错误
        return;
    }

    // ── 第二步：注册 AVRCP Target 回调 ───────────────────
    //
    // AVRCP TG 的 conn_state_changed_cb 只通知连接状态，
    // 实际的按键事件（play/pause）通过 bt_avrcp_tg_register_audio_cbk 注册
    static avrcp_tg_callbacks_t avrcp_cbs = {0};
    avrcp_cbs.conn_state_changed_cb = nullptr; // 暂时不需要处理 AVRCP 连接状态
    avrcp_tg_register_callbacks(&avrcp_cbs);

    // 注册媒体按键事件回调（play/pause/next/prev 等）
    static bt_avrcp_tg_bts_cbk avrcp_media_cbs = {0};
    avrcp_media_cbs.notify_pass_through_status_cbk = avrcp_passthrough_cb;
    bt_avrcp_tg_register_audio_cbk(&avrcp_media_cbs);

    // ── 第三步：启动经典蓝牙协议栈 ────────────────────────
    //
    // enable_bt_stack() 相当于 SLE 里的 enable_sle()
    // 这个调用会触发协议栈内部初始化，完成后调用 bt_stack_state_cb
    errcode_t err = enable_bt_stack();
    if (err != ERRCODE_SUCC) {
        // 启动失败，可能是协议栈已经在运行或者底层初始化失败
        return;
    }
    // 到这里，构造函数结束。
    // 等待 bt_stack_state_cb(BT_TRANSPORT_BR_EDR, BT_STACK_STATE_TURN_ON) 被调用
}

// ============================================================
// 私有函数：设置本地设备信息
// 在 BT stack 完全启动后调用
// ============================================================
void bt::setup_local_device() {
    // 设置设备名（手机蓝牙搜到时显示的名字）
    // 注意：长度参数包含结束符 '\0'，所以要 +1
    bluetooth_set_local_name(
        reinterpret_cast<const unsigned char*>(device_name),
        static_cast<unsigned char>(strlen(device_name) + 1)
    );

    // 设置蓝牙地址（可选，如果不设置则使用芯片默认地址）
    bluetooth_set_local_addr(
        const_cast<unsigned char*>(local_addr),
        sizeof(local_addr)
    );
}

// ============================================================
// 私有函数：设置可发现性
//
// GAP_SCAN_MODE_CONNECTABLE_GENERAL_DISCOVERABLE 的含义：
//   Connectable  = 手机可以向我发起连接请求
//   General Discoverable = 手机扫描时能发现我
//
// 第二个参数 duration 是持续时间（秒），0 表示永久可发现
// ============================================================
void bt::setup_scan_mode() {
    gap_br_set_bt_scan_mode(
        GAP_SCAN_MODE_CONNECTABLE_GENERAL_DISCOVERABLE,
        0  // 永久可发现，直到我们主动关闭或手机连上来
    );
}

// ============================================================
// 私有函数：注册 A2DP 音频流事件监听
//
// bt_register_audio_listener 是整个 A2DP Sink 最核心的函数。
// 它注册一个回调，当以下事件发生时会被调用：
//   - STREAM_CREATE   → A2DP 流被创建
//   - STREAM_OPENED   → 流打开，需要在这里绑定音频端口
//   - STREAM_STRAT    → 流开始传输数据（手机按了播放）
//   - STREAM_SUSPENDED → 流暂停（手机按了暂停）
//   - STREAM_CLOSED   → 流关闭（手机断开连接）
//   - STREAM_CONFIG_CHANGE → Codec 参数发生变化
// ============================================================
void bt::setup_audio_listener() {
    td_u32 ret = bt_register_audio_listener(audio_event_cb, nullptr);
    // nullptr 是 context 参数，会原样传给回调，这里不需要
    (void)ret;
}

// ============================================================
// GAP 回调：BT 协议栈状态变化
//
// 这是最重要的 GAP 回调，等同于 SLE 里的 sle_enable_callback。
// 当 status == BT_STACK_STATE_TURN_ON 时，协议栈完全初始化，
// 这时才能调用各种设置函数。
// ============================================================
void bt::bt_stack_state_cb(const int transport, const int status) {
    // transport: BT_TRANSPORT_BR_EDR(1) 或 BT_TRANSPORT_LE(2)
    // 我们只关心经典蓝牙 BR/EDR
    if (transport != BT_TRANSPORT_BR_EDR) {
        return;
    }

    if (status == BT_STACK_STATE_TURN_ON) {
        // ── 协议栈完全启动！按顺序执行初始化 ────────────
        setup_local_device();    // 1. 设置设备名和地址
        setup_audio_listener();  // 2. 注册音频流事件监听（最重要！）
        setup_scan_mode();       // 3. 开启可发现模式（这一步完成后手机才能搜到）

    } else if (status == BT_STACK_STATE_TURN_OFF) {
        // 协议栈关闭了（通常是调用 disable_bt_stack() 后）
        is_connected = false;
        current_stream_hdl = nullptr;
    }
}

// ============================================================
// GAP 回调：ACL 链路连接状态变化
//
// ACL（Asynchronous Connection-Less）是经典蓝牙的基础数据链路。
// 任何 Profile（A2DP、AVRCP 等）的连接都建立在 ACL 之上。
// 当手机和 WS63 建立/断开基础连接时，这个回调被调用。
// ============================================================
void bt::acl_state_changed_cb(
    const bd_addr_t *bd_addr,
    gap_acl_state_t state,
    unsigned int reason
) {
    (void)reason;
    if (state == GAP_ACL_STATE_CONNECTED) {
        // 手机连上来了！ACL 链路建立
        // 注意：这时候 A2DP 还没有连，只是基础链路通了
        is_connected = true;
        (void)bd_addr;

        // 手机连上来之后，关闭可发现，防止其他手机也连进来
        gap_br_set_bt_scan_mode(GAP_SCAN_MODE_CONNECTABLE, 0);

    } else if (state == GAP_ACL_STATE_DISCONNECTED) {
        // 手机断开了
        is_connected = false;
        current_stream_hdl = nullptr;

        // 清理音频缓冲（调用 iis 的清空函数）
        if (data_clear != nullptr) {
            data_clear();
        }

        // 断开后重新变为可发现，等待下一次连接
        gap_br_set_bt_scan_mode(
            GAP_SCAN_MODE_CONNECTABLE_GENERAL_DISCOVERABLE, 0
        );
    }
}

// ============================================================
// GAP 回调：收到配对请求
//
// 【重要的蓝牙规范】
// 蓝牙配对有多种模式：
//   PIN_CODE  - 老式 4 位数字密码
//   PASSKEY   - 手机显示 6 位数字，你需要通过某种方式确认
//   NUMERIC   - 手机和设备都显示相同数字，用户确认一致
//   CONSENT   - 最简单，只需要用户点击"是否接受配对"
//
// 对于无屏幕的嵌入式音箱，最常用的是自动接受配对。
//
// 【安全考虑】
// 自动接受配对是安全风险！任何手机都能连进来。
// 生产环境应该加入超时限制或硬件按键确认机制。
// ============================================================
void bt::pair_requested_cb(const bd_addr_t *bd_addr) {
    // 自动接受配对请求：true = 接受, false = 拒绝
    gap_br_confirm_pair(bd_addr, true);
}

// ============================================================
// GAP 回调：配对码确认
//
// 某些配对模式下，手机会显示一个数字，需要设备确认。
// req_type 对应 gap_pair_confirm_type_t 枚举值。
// number 是需要确认的数字（PassKey 或 Numeric Comparison）。
// ============================================================
void bt::pair_confirmed_cb(const bd_addr_t *bd_addr, int req_type, int number) {
    // 对于无屏幕音箱，直接确认
    (void)req_type;
    (void)number;
    gap_br_confirm_pair(bd_addr, true);
}

// ============================================================
// GAP 回调：配对状态变化
// ============================================================
void bt::pair_status_changed_cb(const bd_addr_t *bd_addr, int status) {
    (void)bd_addr;
    if (status == GAP_PAIR_PAIRED) {
        // 配对成功，设备已加入配对列表
        // 下次这台手机连接时不需要再配对
    }
}

// ============================================================
// A2DP HAL 核心回调：音频流生命周期事件
//
// 这个回调处理 A2DP 流从创建到关闭的整个过程。
// 每个事件的 data 参数类型不同，必须根据 type 来转换。
//
// 完整事件序列（正常播放）：
//   STREAM_CREATE → STREAM_OPENED → STREAM_CONFIG_CHANGE → STREAM_STRAT
//   [音频播放中...]
//   STREAM_SUSPENDED → STREAM_CLOSED
// ============================================================
void bt::audio_event_cb(
    bt_audio_event_type type,
    const td_void *data,    // 事件携带的数据（每种 type 格式不同）
    int32_t size,           // data 的字节长度
    td_void *context        // 注册时传入的 context（我们传了 nullptr）
) {
    (void)size;
    (void)context;

    switch (type) {

    // ──────────────────────────────────────────────────────
    // 事件 1：STREAM_CREATE
    // 含义：A2DP 流刚被协议栈创建，还没有打开
    // data：stream_hdl（流句柄，类型 td_pvoid*）
    // ──────────────────────────────────────────────────────
    case BT_AUDIO_A2DP_STREAM_CREATE: {
        td_pvoid hdl = *static_cast<const td_pvoid*>(data);
        current_stream_hdl = hdl;
        break;
    }

    // ──────────────────────────────────────────────────────
    // 事件 2：STREAM_OPENED  ← 最重要的事件！
    // 含义：A2DP 流已打开，Codec 协商完成，马上要开始传数据了
    //       这是绑定音频输出端口的时机。
    // data：bt_audio_a2dp_stream_open_data*
    //       {stream_hdl, stream_mtu, frame_size, num_frame}
    //
    // 【为什么在这里绑定端口？】
    // Codec 参数在 OPENED 时才确定（手机和设备在这一步协商结果）。
    // 绑定端口告诉协议栈："把解码出的 PCM 送到 audio_port_id 指定的地方"
    // ──────────────────────────────────────────────────────
    case BT_AUDIO_A2DP_STREAM_OPENED: {
        const auto *open_data = static_cast<const bt_audio_a2dp_stream_open_data*>(data);
        current_stream_hdl = open_data->stream_hdl;

        // 可以在这里查询协商的 Codec 参数（用于调试）
        bt_a2dp_codec_param codec_param = {0};
        bt_get_audio_parameter(
            current_stream_hdl,
            BT_AUDIO_PARAM_A2DP_CODEC,
            &codec_param,
            sizeof(codec_param)
        );
        // codec_param.codec_type: 0=SBC, 1=MPEG12/MP3, 2=AAC, 0xFF=未知

        // ── 绑定音频输出端口 ────────────────────────────
        //
        // bt_audio_port_params 告诉协议栈把 PCM 数据送往何处：
        //   port_type = A2DP  （A2DP 路径，固定这么写）
        //   share_mem_id = 0  （音频路由 ID，0 = 默认 I2S 硬件输出路径）
        //
        // 【share_mem_id 的含义】
        // 在海思 WS63 嵌入式平台，音频路由通过硬件音频总线完成。
        // share_mem_id = 0 很可能对应 I2S 硬件输出路径，此时 BT 协议
        // 栈内部将解码 PCM 直送 I2S DMA，绕过 iis::data_write()，
        // 音频自动从硬件输出，不需要你额外处理。
        bt_audio_port_params port = {0};
        port.port_type    = A2DP;
        port.share_mem_id = audio_port_id;
        bt_attach_audio_port(current_stream_hdl, &port);

        break;
    }

    // ──────────────────────────────────────────────────────
    // 事件 3：STREAM_STRAT（注意：SDK 原文拼写如此，非笔误）
    // 含义：手机按下播放，音频数据开始流入
    // data：stream_hdl（td_pvoid*）
    // ──────────────────────────────────────────────────────
    case BT_AUDIO_A2DP_STREAM_STRAT: {
        if (current_stream_hdl != nullptr) {
            bt_start_audio_stream(current_stream_hdl);
        }
        break;
    }

    // ──────────────────────────────────────────────────────
    // 事件 4：STREAM_SUSPENDED
    // 含义：手机暂停播放
    // data：stream_hdl
    // ──────────────────────────────────────────────────────
    case BT_AUDIO_A2DP_STREAM_SUSPENDED: {
        if (current_stream_hdl != nullptr) {
            bt_pause_audio_stream(current_stream_hdl);
        }
        if (data_clear != nullptr) {
            data_clear();
        }
        break;
    }

    // ──────────────────────────────────────────────────────
    // 事件 5：STREAM_CLOSED
    // 含义：A2DP 流关闭（手机断开或切换）
    // data：stream_hdl
    // ──────────────────────────────────────────────────────
    case BT_AUDIO_A2DP_STREAM_CLOSED: {
        if (current_stream_hdl != nullptr) {
            // 先解绑端口，再停止流
            bt_audio_port_params port = {0};
            port.port_type    = A2DP;
            port.share_mem_id = audio_port_id;
            bt_detach_audio_port(current_stream_hdl, &port);

            bt_stop_audio_stream(current_stream_hdl);
            current_stream_hdl = nullptr;
        }
        if (data_clear != nullptr) {
            data_clear();
        }
        break;
    }

    // ──────────────────────────────────────────────────────
    // 事件 6：STREAM_CONFIG_CHANGE
    // 含义：Codec 参数变化（例如手机切换了 SBC 码率）
    // data：bt_audio_a2dp_config_chg_data*
    //       {stream_hdl, codec（新参数）}
    // ──────────────────────────────────────────────────────
    case BT_AUDIO_A2DP_STREAM_CONFIG_CHANGE: {
        const auto *chg = static_cast<const bt_audio_a2dp_config_chg_data*>(data);
        current_stream_hdl = chg->stream_hdl;
        // 如果需要根据新 codec 调整 IIS 采样率，在这里处理
        break;
    }

    default:
        break;
    }
}

// ============================================================
// AVRCP 媒体按键事件回调
//
// 当手机的媒体控制界面操作时触发：
//   0x44 = PLAY（播放）
//   0x46 = PAUSE（暂停）
//   0x45 = STOP（停止）
//   0x4B = FORWARD（下一首）
//   0x4C = BACKWARD（上一首）
//   0x41 = VOLUME_UP（音量+）
//   0x42 = VOLUME_DOWN（音量-）
//
// key_value：0 = 按下（DOWN），1 = 释放（UP）
// 通常只在按下时执行操作，忽略释放事件。
// ============================================================
void bt::avrcp_passthrough_cb(td_u32 key_operation, td_u32 key_value) {
    // 只处理按下事件，忽略释放
    if (key_value != 0) {
        return;
    }

    switch (key_operation) {
    case 0x44: // PLAY
        if (current_stream_hdl) {
            bt_start_audio_stream(current_stream_hdl);
        }
        // 通知手机：我正在播放（PLAY_STATUS_PLAYING = 0x01）
        avrcp_tg_notify_playback_status_changed(0x01);
        break;

    case 0x46: // PAUSE
        if (current_stream_hdl) {
            bt_pause_audio_stream(current_stream_hdl);
        }
        if (data_clear) {
            data_clear();
        }
        // 通知手机：我已暂停（PLAY_STATUS_PAUSED = 0x02）
        avrcp_tg_notify_playback_status_changed(0x02);
        break;

    case 0x45: // STOP
        if (current_stream_hdl) {
            bt_stop_audio_stream(current_stream_hdl);
        }
        if (data_clear) {
            data_clear();
        }
        // 通知手机：已停止（PLAY_STATUS_STOPPED = 0x00）
        avrcp_tg_notify_playback_status_changed(0x00);
        break;

    default:
        break;
    }
}
```

---

## 第六章：AAC 优先配置（让 iOS 手机优先使用 AAC）

如果想优先使用 AAC（音质优于 SBC，iOS 设备默认支持），可以在流创建后、打开前设置偏好：

```cpp
// 在 audio_event_cb 的 STREAM_CREATE 事件中调用
void bt::prefer_aac_codec() {
    if (current_stream_hdl == nullptr) {
        return;
    }

    bt_a2dp_codec_param aac_param = {0};
    aac_param.codec_type = BT_AUDIO_CODEC_MPEG24;  // 0x02 = AAC
    aac_param.cap_len    = sizeof(bt_a2dp_mpeg24_codec_caps);

    bt_a2dp_mpeg24_codec_caps *aac_caps =
        reinterpret_cast<bt_a2dp_mpeg24_codec_caps*>(aac_param.codec_caps);
    aac_caps->sample_frequency = BT_AUDIO_A2DP_SMAPLE_RATE44100; // 44100 Hz
    aac_caps->channels         = BT_AUDIO_A2DP_AAC_CH_2;         // 立体声
    aac_caps->object_type      = BT_AUDIO_A2DP_AAC_MPEG4_LC;     // AAC-LC（最通用）
    aac_caps->vbr              = BT_AUDIO_A2DP_AAC_NOVBR;        // 固定码率
    aac_caps->bit_rate         = 256000;                          // 256 kbps

    bt_set_audio_parameter(
        current_stream_hdl,
        BT_AUDIO_PARAM_A2DP_CODEC,
        &aac_param,
        sizeof(aac_param)
    );
    // 注意：这只是声明偏好，最终用什么 Codec 由手机决定
    // 如果手机不支持 AAC，会回退到 SBC
}
```

---

## 第七章：与 main 集成

```cpp
#include "includes/iis/iis.hpp"
#include "includes/bt/bt.hpp"

int main() {
    // ── 1. 初始化 IIS DMA 输出 ────────────────────────────
    iis iis_instance;

    // ── 2. 初始化蓝牙 A2DP Sink ──────────────────────────
    // 如果 BT 协议栈直接路由音频到硬件 I2S（share_mem_id=0），
    // 则无需设置 data_process，音频自动从硬件出来。
    bt bt_instance;

    // ── 3. 可选：如果需要手动接管 PCM ─────────────────────
    // bt::set_data_clear_function(iis::data_clear);

    while (true) {
        osal_msleep(100);
    }
    return 0;
}
```

---

## 第八章：蓝牙协议规范约束——你必须遵守的公约

### 8.1 配对安全规范

- **首次连接必须配对：** 蓝牙协议要求首次建立安全连接前必须完成配对，配对后双方保存密钥，再次连接时自动恢复
- **不能拒绝所有配对：** 如果 `pair_requested_cb` 中一律拒绝，手机永远无法连接
- **配对完成后建议限制可发现：** 已配对设备可以直接连接，继续广播会让陌生设备也能来连

### 8.2 Codec 协商规范

- **你无法强制手机使用某种 Codec：** `bt_set_audio_parameter` 只是声明你的偏好，最终用什么 Codec 由手机决定
- **SBC 必须接受：** 按照蓝牙 A2DP 规范，你不能拒绝 SBC，必须支持
- **协商失败会自动降级：** 如果手机只支持 SBC，即使你声明支持 AAC，也会以 SBC 完成连接

### 8.3 流控制规范

- **手机控制流的开/关：** `STREAM_STRAT`、`STREAM_SUSPENDED` 事件是手机发起的，你只能响应
- **你可以通过 AVRCP TG 通知手机状态：** `avrcp_tg_notify_playback_status_changed()` 可以告诉手机当前播放状态，手机的锁屏控制界面会更新

### 8.4 连接管理规范

- **蓝牙地址是唯一标识：** 手机通过蓝牙 MAC 地址记住你的设备，如果地址每次启动都变，手机会认为是新设备，每次都要重新配对
- **断连后协议栈自动保留配对信息：** 已配对设备下次可以直接连接
- **手机可能自动重连：** iOS 和 Android 都会在蓝牙开启时自动尝试连接上次配对的设备

---

## 第九章：完整初始化时序图

```
WS63 上电
    │
    ▼
bt::bt() 构造函数
    │── gap_register_callbacks()            注册 GAP 回调
    │── avrcp_tg_register_callbacks()       注册 AVRCP TG 连接状态回调
    │── bt_avrcp_tg_register_audio_cbk()    注册媒体按键回调
    └── enable_bt_stack()                   异步启动协议栈
    │
    │  （等待协议栈内部初始化...）
    │
    ▼
bt_stack_state_cb(BR_EDR, TURN_ON)          协议栈就绪
    │── setup_local_device()                设置设备名和地址
    │── setup_audio_listener()              注册 bt_register_audio_listener
    └── setup_scan_mode()                   GAP_SCAN_MODE_CONNECTABLE_GENERAL_DISCOVERABLE
    │
    │  （等待手机发现并连接...）
    │
    ▼
pair_requested_cb()                         手机发起配对
    └── gap_br_confirm_pair(addr, true)     自动接受
    │
    ▼
acl_state_changed_cb(CONNECTED)             ACL 链路建立
    └── gap_br_set_bt_scan_mode(CONNECTABLE) 关闭广播，防止别的手机连
    │
    │  （A2DP Codec 协商，协议栈自动处理...）
    │
    ▼
audio_event_cb(STREAM_CREATE)               流创建，保存 stream_hdl
    │
    ▼
audio_event_cb(STREAM_OPENED)               Codec 协商完成，流打开
    │── bt_get_audio_parameter()            查询实际 Codec（调试用）
    └── bt_attach_audio_port()              绑定 I2S 音频输出端口
    │
    │  （手机用户按播放...）
    │
    ▼
audio_event_cb(STREAM_STRAT)                音频流开始
    └── bt_start_audio_stream()             启动解码管线
    │
    │  （持续接收音频帧，协议栈解码后路由到 I2S 硬件...）
    │                ↓
    │           [音频输出 🎵]
    │
    │  （手机用户按暂停...）
    │
    ▼
audio_event_cb(STREAM_SUSPENDED)
    │── bt_pause_audio_stream()
    └── data_clear()                        清空缓冲
```

---

## 第十章：常见问题排查

| 问题 | 可能原因 | 解决方案 |
|------|---------|---------|
| 手机搜不到设备 | `gap_br_set_bt_scan_mode` 没有调用或调用时机不对 | 确认在 `BT_STACK_STATE_TURN_ON` 之后调用 |
| 手机能搜到但无法连接 | `pair_requested_cb` 中拒绝了配对 | 确认 `gap_br_confirm_pair(addr, true)` 被调用 |
| 配对成功但没有声音 | `share_mem_id = 0` 不对，或端口未绑定 | 确认 `bt_attach_audio_port` 在 `STREAM_OPENED` 中调用；尝试不同 `share_mem_id` 值 |
| 声音断断续续 | I2S 时钟与 A2DP 时钟不同步 | 确认 IIS 采样率与 Codec 协商结果一致 |
| 只有 SBC，无法用 AAC | 手机不支持 AAC | 先确认 SBC 工作，AAC 是可选的 |
| 手机每次都要重新配对 | 蓝牙地址不固定或配对信息未持久化 | 固定蓝牙地址（每次相同值） |
| AVRCP 按键无响应 | `bt_avrcp_tg_register_audio_cbk` 未注册 | 确认构造函数中注册了 `avrcp_media_cbs` |

---

## 附录：CMakeLists.txt 配置

```cmake
target_include_directories(your_target PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/middleware/services/bts/br
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/middleware/services/bts
)

target_link_libraries(your_target PRIVATE bth_sdk)
```

---

*本教程基于 WS63 SDK `include/middleware/services/bts/br/` 目录下的头文件分析编写。*
*头文件版权：HiSilicon (Shanghai) Technologies Co., Ltd. 2022*

---

## 第11章：L2HC 编解码器实战——发送端与接收端全解析

### 11.1 首先必须知道：L2HC 在哪里运行？

在开始任何代码之前，需要厘清一个根本性问题：**L2HC 编解码器运行在哪一层？**

对 WS63 SDK 的全量分析给出了确定答案：

```
搜索范围         结果
─────────────────────────────────────────────────
全部 3728 个 .h  无任何 L2HC 函数声明（Init/Encode/Decode）
全部 198 个 .a   无任何 L2HC 符号（nm 扫描结果为 0）
CMakeLists.txt   无 l2hc 链接规则
──────────────────────────────────────────────
结论             L2HC 100% 实现在 BGTP 控制器 ROM 固件中
```

`build/config/target_config/common_config.py` 中的 `codec_set`：

```python
'codec_set': ['l2hc_dec_16k', 'l2hc_dec_48k_10ms', 'l2hc_dec_48k_5ms',
              'l2hc_enc_16k', 'l2hc_enc_48k_10ms', 'l2hc_enc_48k_5ms', ...]
```

这些组件名是**控制器固件的构建模块**——构建系统把它们打包进控制器二进制，但它们**不是**可链接的用户空间库。你无法 `#include` 也无法直接调用任何 L2HC 函数。

> **一句话总结：** L2HC 运行在硬件控制器里，上层应用与它交互的唯一方式是通过协议栈的配置接口（A2DP Codec Cap 或 SLE Audio Profile），而不是直接调用编解码器 API。

---

### 11.2 L2HC 在 A2DP（经典蓝牙 BR/EDR）中的工作方式

#### 控制器自动处理的完整链路

```
发送端（Source，如手机）                 接收端（Sink，即你的 WS63 设备）
─────────────────────────────────────────────────────────────────────
① PCM 音频数据
② 调用 L2HC 编码（控制器内部完成）
③ A2DP Vendor Specific 帧（0xFF）──────► 空中传输
                                         ④ 控制器 ROM 固件自动解码 L2HC
                                         ⑤ PCM 数据
                                         ⑥ a2dp_sink_audio_data_cb 回调给用户
                                         ⑦ bt_attach_audio_port 写入 I2S
```

你在第5章实现的 A2DP Sink 代码对此完全透明——控制器在回调之前已经解码好了。你唯一需要做的是**正确配置 Codec Cap**，告诉对端"我支持 Vendor Specific（L2HC）"。

#### Codec Cap 配置（接收端）

在 `a2dp_sink_register()` 时，填写 `a2dp_codec_ability_t`：

```c
a2dp_codec_ability_t ability = {0};
ability.codec_type = BT_AUDIO_CODEC_UNKNOWN;  // 0xFF = Vendor Specific = L2HC/LHDC 通道

// codec_caps[16] 的前 4 字节是 Vendor Company ID
// HiSilicon L2HC 的 Company ID（以实际硬件协商为准，此处为示例结构）
ability.codec_caps[0] = 0xD7;  // Company ID LSB（示例）
ability.codec_caps[1] = 0x04;  // Company ID MSB（示例）
ability.codec_caps[2] = 0x00;  // Codec Version
ability.codec_caps[3] = 0x00;
// 后续字节为采样率、位深等能力描述，由控制器ROM固件自动解析
```

> **注意：** 实际的 HiSilicon L2HC Vendor Company ID 由控制器固件定义，不对外公开。
> **实用做法：** 只启用 SBC + AAC（`BT_AUDIO_CODEC_SBC` + `BT_AUDIO_CODEC_MPEG24`），
> 控制器 ROM 自动处理 L2HC 的 Vendor Specific 协商，无需手动填写 `codec_caps`。
> 若对端（手机/PC）也支持 L2HC，控制器会优先选用。

#### A2DP Sink 接收端代码摘要（与第5章相同，L2HC 对用户透明）

```cpp
// 音频数据回调——无论对端发的是 SBC/AAC/L2HC，到这里都已经是 PCM 了
static void on_audio_data(const uint8_t *data, uint32_t data_len,
                          const a2dp_codec_info_t *codec_info) {
    // codec_info->codec_type 告诉你实际协商用的是哪种编解码器
    if (codec_info->codec_type == BT_AUDIO_CODEC_UNKNOWN) {
        // 这意味着对端使用了 Vendor Specific（可能是 L2HC 或 LHDC）
        // 但数据已经是 PCM——控制器已经解码好了
    }
    // 直接送 I2S，无需任何额外处理
    iis::data_write(reinterpret_cast<const int16_t*>(data),
                    data_len / sizeof(int16_t));
}
```

**结论：** 在 A2DP Sink 路径上，L2HC **对应用层完全透明**，你什么都不需要改。

---

### 11.3 L2HC 在星闪（SLE）中的架构分析

#### 你当前的 SLE 实现架构（原始 PCM 透传）

```
发送端（sending end）                    接收端（receiving end，即你的 WS63）
───────────────────────────────────────────────────────────────────
PCM 采样 (int16_t[])
│
ssapc_write_req() ──────────────────────► ssaps 回调 get_data_callback()
                                          │
                                          data_process(value, length)
                                          │
                                          iis::data_write(pcm, len)
```

**优点：** 零延迟、零质量损失、实现最简单  
**代价：** 带宽需求高（44100 Hz · 2ch · 16bit = 1.41 Mbps 净载荷）

#### SLE 的带宽实测估算

你的 SLE 参数（`sle.hpp`）：

| 参数 | 值 | 含义 |
|------|-----|------|
| `SLE_PHY_4M` | 4M | 调制速率 4 Mbps |
| `SLE_RADIO_FRAME_2` | 帧格式2 | 海思星闪私有帧 |
| `max_mtu` | 800 字节 | 单包最大净载荷 |
| `conn_interval` | 0x14 | 星闪连接间隔（单位 125 μs × 20 = 2.5 ms） |

理论峰值吞吐：`800 bytes / 2.5 ms = 320,000 bytes/s ≈ 2.56 Mbps`  
44100 Hz 立体声 16-bit 净需求：`176,400 bytes/s ≈ 1.41 Mbps`

> **结论：** 在 4M PHY 下，SLE 的实际可用带宽**大约是原始 PCM 需求的 1.8 倍**，
> 理论上原始 PCM 透传是可行的。实际上还需考虑协议开销（约 30%），所以是否压缩
> 取决于你的实际测试结果是否有丢包/卡顿。

#### L2HC 在 SLE 中的真实使用路径

L2HC 在 SLE 中工作的方式有两种，但本 SDK 版本只有一种可用：

| 路径 | 描述 | 本 SDK 是否可用 |
|------|------|----------------|
| **路径 A：SLE Audio Profile**（等时信道） | 控制器自动处理 L2HC 编解码，用户空间仅处理 PCM | ❌ 本 SDK 未暴露 `sle_audio_*.h` |
| **路径 B：SSAP + 手动压缩** | 用户空间编码压缩帧，通过 ssapc_write_req 发送 | ✅ 可行，但 L2HC API 不可用 |
| **路径 C：SSAP + 第三方软件编解码器** | 用 开源软件编解码器（如 minimp3、ADPCM）实现压缩 | ✅ 完全可行 |

> **SDK 现实：** `include/middleware/services/bts/sle/` 目录中没有 `sle_audio_*.h` 或
> 等时信道（Isochronous Channel）相关头文件。SLE Audio Profile 是企业/NDA 版 SDK 功能，
> 当前开源 WS63 SDK 未包含该接口。

---

### 11.4 如果需要压缩：在 SSAP 架构上叠加压缩的实现方案

由于 L2HC 的直接 API 不可用，以下给出**在你现有 SLE SSAP 架构上实现压缩**的完整方案。

#### 方案 A：ADPCM 软件压缩（推荐，最简单）

ADPCM（自适应差分 PCM）是嵌入式最常用的软件压缩算法：
- 压缩比 4:1（16-bit PCM → 4-bit ADPCM）
- 无外部依赖，代码量约 200 行 C
- 延迟 < 1ms，CPU 开销极低（RISC-V 32 上约 2% CPU）

**接收端修改（`sle.cpp` → `main.cpp` 的 data_process）：**

```cpp
// main.cpp 或 receiving end 的集成代码

// ① 包含 ADPCM 解码器（单头文件实现）
#include "adpcm.h"  // 自行移植，约 150 行

// ② 数据处理回调（替换原有的直接 data_write）
static void sle_audio_data_process(const uint8_t *data, uint16_t length) {
    // data 收到的是 ADPCM 压缩帧
    // 每帧格式：[4字节帧头：采样数(2B) + 状态(2B)] + [ADPCM数据]
    
    static int16_t pcm_buf[960 * 2];  // 最大 960 样本立体声
    
    // 解压 ADPCM → PCM
    int samples = adpcm_decode(data, length, pcm_buf, sizeof(pcm_buf));
    if (samples > 0) {
        iis::data_write(pcm_buf, samples);
    }
}

// ③ 注册回调
sle::set_data_process_fuction(sle_audio_data_process);
```

**发送端对应编码（sending end 的 SLE 客户端）：**

```cpp
// sending end 的 SLE 发送逻辑

// 捕获 PCM 后，编码再发送
static void send_audio_frame(const int16_t *pcm, uint32_t samples) {
    static uint8_t adpcm_buf[512];
    
    // 编码 PCM → ADPCM（压缩比 4:1）
    int adpcm_len = adpcm_encode(pcm, samples, adpcm_buf, sizeof(adpcm_buf));
    
    // 通过 SSAP 发送压缩帧
    ssapc_write_req(conn_id, service_handle, property_handle,
                    adpcm_buf, adpcm_len);
}
```

#### 方案 B：自定义差分编码（零依赖，极简）

如果不想引入任何外部代码，可以用最简单的 delta 编码：

```cpp
// ===================== 接收端 =====================
// 接收差分编码数据，还原 PCM
static void sle_delta_audio_process(const uint8_t *data, uint16_t length) {
    static int16_t last_l = 0, last_r = 0;
    static int16_t pcm_buf[800];
    
    int16_t *p = pcm_buf;
    // 每个差值用 8-bit 有符号数表示（压缩比 2:1）
    for (uint16_t i = 0; i + 1 < length; i += 2) {
        last_l += (int8_t)data[i];      // 左声道差值还原
        last_r += (int8_t)data[i + 1];  // 右声道差值还原
        *p++ = last_l;
        *p++ = last_r;
    }
    iis::data_write(pcm_buf, (uint16_t)(p - pcm_buf));
}
```

> 注意：差分编码仅适合变化缓慢的音频信号，对高频信号（如钢琴、打击乐）效果差。

#### 方案 C：保持原始 PCM（如果带宽够用）

如果你的实际测试中**没有出现**卡顿、丢包、重传超时等问题，**直接保持原始 PCM 传输是最好的选择**。L2HC 相对原始 PCM 的唯一优势是节省带宽，而不是提升音质。

```cpp
// 保持现有代码不变，无需任何修改
static void sle_raw_pcm_process(const uint8_t *data, uint16_t length) {
    iis::data_write(
        reinterpret_cast<const int16_t*>(data),
        length / sizeof(int16_t)
    );
}
```

---

### 11.5 针对不同场景的完整推荐方案

| 你的场景 | 推荐方案 | 原因 |
|----------|----------|------|
| 双 WS63 点对点，SLE 带宽充足（无卡顿） | **保持原始 PCM** | 零复杂度，零质量损失 |
| 双 WS63 点对点，发现偶尔卡顿/丢帧 | **ADPCM 压缩（方案A）** | 4:1 压缩，减少带宽至 352 kbps |
| WS63 作为 A2DP Sink，接收手机音乐 | **A2DP Sink（第5章代码）** | L2HC 由控制器自动处理，无需干预 |
| WS63 作为 A2DP Sink，想要 L2HC 优先 | **保持 Vendor Specific 能力注册** | 若手机支持 L2HC，控制器自动协商 |
| 需要 SLE Audio Profile（L2HC native） | **需要企业版 SDK** | 当前开源 SDK 未包含该接口 |

---

### 11.6 发送端完整集成示例

以下是发送端（`sending end`）集成 ADPCM 压缩的完整骨架：

```cpp
// sending_end_audio.cpp

#include <cstdint>
#include "adpcm.h"   // 移植的 ADPCM 编码器

// 帧参数：48 kHz, 5ms 帧 = 240 样本/帧/声道
static constexpr uint32_t FRAME_SAMPLES   = 240;  // 每声道样本数
static constexpr uint32_t PCM_FRAME_BYTES = FRAME_SAMPLES * 2 * 2;   // 双声道 16-bit
static constexpr uint32_t ADPCM_FRAME_BYTES = PCM_FRAME_BYTES / 4;   // 4:1 压缩

static uint8_t  adpcm_frame[ADPCM_FRAME_BYTES + 4];  // +4 帧头
static int16_t  pcm_accumulate[FRAME_SAMPLES * 2];   // 累积缓冲
static uint32_t accumulated_samples = 0;

// 从 ADC/DMA 捕获 PCM 后调用此函数
void on_pcm_captured(const int16_t *pcm, uint32_t samples_per_ch) {
    uint32_t to_copy = samples_per_ch;
    while (to_copy > 0) {
        uint32_t space = FRAME_SAMPLES - accumulated_samples;
        uint32_t copy  = (to_copy < space) ? to_copy : space;
        
        // 拷贝双声道交织 PCM
        for (uint32_t i = 0; i < copy; ++i) {
            pcm_accumulate[(accumulated_samples + i) * 2]     = pcm[i * 2];
            pcm_accumulate[(accumulated_samples + i) * 2 + 1] = pcm[i * 2 + 1];
        }
        accumulated_samples += copy;
        to_copy             -= copy;
        
        if (accumulated_samples == FRAME_SAMPLES) {
            // 一帧凑满，编码后发送
            int adpcm_len = adpcm_encode(pcm_accumulate, FRAME_SAMPLES * 2,
                                         adpcm_frame + 4, sizeof(adpcm_frame) - 4);
            // 写 4 字节帧头（采样数 + 保留）
            adpcm_frame[0] = (uint8_t)(FRAME_SAMPLES & 0xFF);
            adpcm_frame[1] = (uint8_t)(FRAME_SAMPLES >> 8);
            adpcm_frame[2] = 0x00;
            adpcm_frame[3] = 0x00;
            
            // 通过 SLE SSAP 发送
            sle_client_send(adpcm_frame, adpcm_len + 4);
            
            accumulated_samples = 0;
        }
    }
}
```

---

### 11.7 接收端完整集成示例

对应修改 `main` 中注册给 `sle` 的 `data_process` 回调：

```cpp
// main/receiving end/main.cpp（集成部分）

#include "adpcm.h"
#include "iis.hpp"
#include "sle.hpp"

// 解压缓冲：ADPCM 帧最大还原出 960 双声道样本 = 3840 字节
static int16_t pcm_decode_buf[960 * 2];

static void sle_audio_process(const uint8_t *data, uint16_t length) {
    if (length < 4) return;  // 至少要有帧头
    
    uint16_t expected_samples = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    
    // 跳过 4 字节帧头，解码 ADPCM 载荷
    int decoded = adpcm_decode(data + 4, length - 4,
                               pcm_decode_buf,
                               (int)(expected_samples * 2));  // 双声道
    if (decoded > 0) {
        iis::data_write(pcm_decode_buf, (uint16_t)decoded);
    }
}

static void sle_audio_clear() {
    iis::data_clear();
}

// 在 main 或初始化函数中注册
void app_audio_init() {
    sle::set_data_process_fuction(sle_audio_process);
    sle::set_data_clear_fuction(sle_audio_clear);
    
    // 创建 sle 实例（触发 enable_sle() 和广播）
    static sle sle_instance;
}
```

---

### 11.8 关键结论速查

```
┌─────────────────────────────────────────────────────────────────┐
│  L2HC 在 WS63 SDK 中的使用规则（终极总结）                       │
├─────────────────────────────────────────────────────────────────┤
│  A2DP Sink（经典蓝牙）                                          │
│    ✅ L2HC 由控制器 ROM 自动处理                                │
│    ✅ 用户空间零感知，数据回调直接是 PCM                        │
│    ✅ 注册 codec_type = BT_AUDIO_CODEC_UNKNOWN 即可参与协商     │
├─────────────────────────────────────────────────────────────────┤
│  SLE SSAP（星闪通用数据通道，你的当前实现）                     │
│    ❌ 无法直接调用 L2HC API（SDK 未暴露）                       │
│    ✅ 可用软件 ADPCM 等代替（带宽不足时）                      │
│    ✅ 带宽充足时无需压缩（保持原始 PCM 最佳）                  │
├─────────────────────────────────────────────────────────────────┤
│  SLE Audio Profile（等时信道，native L2HC）                     │
│    ❌ 当前开源 SDK 未包含该接口                                 │
│    ℹ️  需要海思企业版/NDA SDK 才能访问                          │
└─────────────────────────────────────────────────────────────────┘
```

> **对你而言最实用的结论：**
> - 你的 SLE 实现当前是完全无损的原始 PCM 传输，这是**最理想的质量**
> - 只有当你测试中发现带宽不足（卡顿、掉帧）时才需要考虑 ADPCM 压缩
> - L2HC 的 native SLE 使用需要企业版 SDK，在当前 WS63 开源 SDK 中无法实现
> - 在 A2DP Sink 场景下，L2HC 对你的代码完全透明，控制器全权处理

