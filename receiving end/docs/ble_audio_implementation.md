# WS63 BR/EDR A2DP Sink 蓝牙音频接收 — 详细实现方案

> **基于：** SDK 实际头文件深度穷举验证 + 构建系统全量分析（见 `ble_audio_verification.md`）
> **深度排查结论：** 核心架构可行。1 个致命问题（`gap_br_confirm_pair` 不存在，SDK 默认自动接受配对），5 个重要发现（详见文档内标注）。
> **目标：** 让 WS63 作为蓝牙音箱（A2DP Sink），手机可直接在蓝牙设置中连接并播放音乐
> **约束：** **只修改蓝牙部分**（`includes/ble/` + `realize/ble_task/` + 相关 CMakeLists），**不动 iis 等其他模块**

---

## 〇、实现范围与文件清单

### 需要新建/修改的文件

```
main/receiving end/
├── includes/ble/
│   ├── ble.hpp          ← 重写：BR/EDR A2DP Sink 封装类声明
│   ├── ble.cpp          ← 重写：BR/EDR A2DP Sink 封装类实现
│   └── CMakeLists.txt   ← 修改：增加 include 路径
├── realize/ble_task/
│   ├── ble_task.hpp     ← 修改：增加初始化声明
│   ├── ble_task.cpp     ← 重写：ble 任务入口
│   └── CMakeLists.txt   ← 不变（已正确）
├── realize/CMakeLists.txt ← 修改：增加 ble_task 子目录
├── includes/CMakeLists.txt ← 不变（已包含 ble 子目录）
└── docs/
    └── ble_audio_implementation.md  ← 本文档
```

### 不需要修改的文件

- `iis.hpp` / `iis.cpp` — 不变
- `sle.hpp` / `sle.cpp` — 不变
- `main.cpp` — 可能需要加一行 `#include` + 一行创建 `ble_task` 线程，但**不修改其现有逻辑**

---

## 一、整体架构设计

```
┌──────────────────────────────────────────────────────────────────┐
│                        main.cpp                                  │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────────────┐ │
│  │ led_task  │  │ wifi_task │  │miniMP3_t │  │ ble_task (NEW)  │ │
│  └──────────┘  └──────────┘  └──────────┘  └────────┬────────┘ │
│                                                      │          │
│  ┌───────────────────────────────────────────────────┤          │
│  │                ble 模块 (includes/ble/)           │          │
│  │                                                   │          │
│  │  ┌─────────────────────────────────────────────┐  │          │
│  │  │           bt 类 (静态 C++ 封装)              │  │          │
│  │  │                                             │  │          │
│  │  │  构造函数:                                  │  │          │
│  │  │    ① gap_register_callbacks()   GAP 回调    │  │          │
│  │  │    ② avrcp_tg_register_callbacks()         │  │          │
│  │  │    ③ bt_avrcp_tg_register_audio_cbk()      │  │          │
│  │  │    ④ enable_bt_stack()                      │  │          │
│  │  │                                             │  │          │
│  │  │  回调链 (全部静态函数):                     │  │          │
│  │  │    bt_stack_state_cb ─► setup 系列          │  │          │
│  │  │    audio_event_cb ─► 流生命周期管理         │  │          │
│  │  │    avrcp_passthrough_cb ─► 媒体按键         │  │          │
│  │  │    pair_*_cb ─► 配对处理                    │  │          │
│  │  └─────────────────────────────────────────────┘  │          │
│  │                       │                           │          │
│  │   data_process / data_clear 回调 ──► iis 模块     │          │
│  └───────────────────────────────────────────────────┘          │
└──────────────────────────────────────────────────────────────────┘
```

### 设计原则

1. **全静态类** — 与 `sle.hpp` 风格一致，所有成员都是 `static`
2. **回调驱动** — 蓝牙是异步协议栈，所有逻辑在回调中完成
3. **最小侵入** — 只通过 `data_process`/`data_clear` 函数指针与外部交互
4. **share_mem_id = 0 优先** — 先走硬件直通路径（BT 解码 → I2S 硬件），如不工作再走共享内存手动路径

---

## 二、音频数据流路径选择

### 路径 A：硬件直通（首选，零 CPU 开销）

```
手机 SBC/AAC 数据 ──► 控制器固件解码 ──► PCM ──► bt_attach_audio_port(share_mem_id=0)
                                                            │
                                                    I2S 硬件 DMA
                                                            │
                                                       扬声器 🔊
```

- **优点：** 无需用户代码干预，延迟最低
- **条件：** `share_mem_id = 0` 对应正确 I2S 路由
- **此时 `data_process` 回调不会被调用**

### 路径 B：共享内存手动输出（备选）

```
手机 SBC/AAC 数据 ──► 控制器固件解码 ──► PCM ──► 共享内存
                                                        │
                                              data_process 回调
                                                        │
                                              iis::data_write(pcm, len)
                                                        │
                                                   扬声器 🔊
```

- **优点：** 用户完全控制 PCM 数据
- **需要：** 找到正确的 `share_mem_id` 值

**实现策略：** 代码同时支持两条路径。`data_process` 回调在需要时注册，不需要时留 `nullptr`。

---

## 三、深度排查发现的全部问题与修正

### 🔴 致命问题 1：`gap_br_confirm_pair()` 不存在

**穷举 `bts_br_gap.h` 全部 24 个导出函数，无任何配对确认 API。**

- `gap_pair_requested_callback` — 纯通知回调，`void (*)(const bd_addr_t*)`，无输出参数
- `gap_pair_confirmed_callback` — 纯通知回调，`void (*)(const bd_addr_t*, int, int)`，无输出参数
- `gap_is_accept_conn_on_safe_mode_callback` — 控制 **ACL 连接**接受（非配对确认），有 `bool *res` 输出参数

**结论：SDK 在协议栈内部默认自动接受所有配对请求。回调仅用于通知上层。** 教程中 `gap_br_confirm_pair(bd_addr, true)` 调用必须删除。

### 🟡 重要发现 2：BR/EDR 栈已完整启用

验证结果：
- `ws63-liteos-app` target 的 `ram_component` 含 `bt_host`, `bth_sdk`, `bgtp`, `bt_app`
- `BT_GLE_ONLY` 宏**未定义**（意味着完整 BR/EDR + BLE）
- `libbt_host.a` = **1.1 MB**（若仅 GLE/BLE 只会 ~200KB）
- `hso_enable_bt: True` 确认 HSO ROM/RAM 分区包含了 BT

**结论：不需要修改 Kconfig 或添加库链接，BR/EDR 栈已就绪。**

### 🟡 重要发现 3：无独立 `bts_a2dp_sink.h`

SDK 只有 `bts_a2dp_source.h`（Source 角色，手机端）。Sink 角色（音箱端）全部通过 `bt_audio_hal_interface.h` 的 `bt_register_audio_listener()` 实现。注册监听器后，协议栈自动：
1. 注册 A2DP Sink SDP Service Record
2. 设置正确的 CoD (Class of Device)
3. 准备好接受 A2DP Source 的连接

**结论：教程方案正确，不需要额外注册 A2DP Sink Profile。**

### 🟡 重要发现 4：`bt_avrcp_tg_register_audio_cbk` 返回 `td_void`

该函数**不返回错误码**，无法判断注册成功/失败。调用后直接假设成功即可。

### 🟡 重要发现 5：SDK 注释中 `OPENED` 事件的 size 描述矛盾

SDK 源码注释：
```c
BT_AUDIO_A2DP_STREAM_OPENED, /* 数据为 bt_audio_a2dp_stream_open_data，长度为sizeof(td_pvoid) */
```

"长度为 sizeof(td_pvoid)" 是注释错误 — 实际应传 `bt_audio_a2dp_stream_open_data` 结构体。代码按结构体指针转换是正确的。

### 🟡 重要发现 6：所有 BT 回调运行在 "bts context"

这意味着回调中**不能阻塞或长时间等待**。但 `bt_attach_audio_port()`、`bt_start_audio_stream()` 等属于 BTS 内部 API，可安全在回调上下文中调用。

### ✅ 验证通过的 6 项

| 验证项 | 结论 |
|:--|:--|
| `bt_audio_listener_cb` 签名 | 参数类型、返回值 (`td_void`) 完全匹配 |
| 6 个 A2DP 事件枚举 | 全部存在（含 SDK 拼写 `STRAT`） |
| `bd_addr_t` = `{uint8_t addr[6]; uint8_t type;}` | 7 字节结构体，教程正确使用 |
| `gap_call_backs_t` 字段 typo | 教程正确沿用 SDK 的 `callbak` / `confiremed` |
| `bt_audio_port_params` 匿名 union | C++ 合法，`share_mem_id` 访问正常 |
| `bt_get_audio_parameter` 参数匹配 | `BT_AUDIO_PARAM_A2DP_CODEC` + `bt_a2dp_codec_param` 正确 |

---

## 四、配对确认问题的实际处理

由于 `gap_br_confirm_pair()` 不存在且 SDK 默认自动接受配对：

```cpp
// pair_requested_cb — SDK 自动接受配对，这里仅做通知日志
static void pair_requested_cb(const bd_addr_t *bd_addr) {
    (void)bd_addr;
    // 如果实测发现配对失败，可尝试注册 is_accept_conn_on_safe_mode_callback
}

// pair_confirmed_cb — 同上
static void pair_confirmed_cb(const bd_addr_t *bd_addr, int req_type, int number) {
    (void)bd_addr; (void)req_type; (void)number;
}
```

**备用方案：** 如果实测配对不工作，在 `gap_call_backs_t` 中注册：
```cpp
static void accept_all_connections(const bd_addr_t *bd_addr, bool *res) {
    *res = true;
}
// gap_cbs.is_accept_conn_on_safe_mode_callback = accept_all_connections;
```

---

## 四、分步实现计划

### Step 1：重写 `ble.hpp` — 类声明

**文件：** `main/receiving end/includes/ble/ble.hpp`

核心内容：
- `bt` 类（全静态成员）
- GAP 回调声明（6 个）
- A2DP 音频事件回调（1 个）
- AVRCP 媒体按键回调（1 个）
- 初始化函数（3 个 setup）
- 数据注入接口（`set_data_process_function` / `set_data_clear_function`）

### Step 2：重写 `ble.cpp` — 类实现

**文件：** `main/receiving end/includes/ble/ble.cpp`

核心实现顺序：
1. 静态成员初始化
2. 构造函数 → 注册所有回调 + `enable_bt_stack()`
3. `bt_stack_state_cb` → `setup_local_device()` → `setup_audio_listener()` → `setup_scan_mode()`
4. `audio_event_cb` → 处理 STREAM_CREATE/OPENED/STRAT/SUSPENDED/CLOSED/CONFIG_CHANGE
5. `avrcp_passthrough_cb` → PLAY/PAUSE/STOP 按键响应
6. GAP 回调：ACL 状态变化、配对请求通知

### Step 3：修改 `ble_task.cpp` — 任务入口

**文件：** `main/receiving end/realize/ble_task/ble_task.cpp`

```cpp
#include "ble_task.hpp"

void ble_task(void *arg) {
    unused(arg);
    
    // 创建 bt 实例（构造函数自动完成所有初始化）
    static bt bt_instance;
    
    // 可选：如果走共享内存路径，注册数据回调
    // bt::set_data_process_function(iis::data_write);
    // bt::set_data_clear_function(iis::data_clear);
    
    // 蓝牙完全是事件驱动的，任务主体只需保持存活
    while (true) {
        osal_msleep(1000);
    }
}
```

### Step 4：修改 CMakeLists.txt 系列

**`includes/ble/CMakeLists.txt`：**
```cmake
# 增加蓝牙头文件搜索路径
target_include_directories(... 
    ${CMAKE_SOURCE_DIR}/include/middleware/services/bts/br
    ${CMAKE_SOURCE_DIR}/include/middleware/services/bts/common
)
```

**`realize/CMakeLists.txt`：** 增加 `add_subdirectory(ble_task)`。

### Step 5：集成到 `main.cpp`

在 `app_entry()` 中增加一行创建 ble 线程：
```cpp
taskid = osal_kthread_create((osal_kthread_handler)ble_task, NULL, "ble_task", 4096);
(void)taskid;
```

---

## 六、Kconfig 与编译链接（无需修改！）

### 6.1 Kconfig 状态

**无需任何修改。** 深度排查确认：

- `ws63-liteos-app` target 的 `ram_component` 已包含 `bt_host`, `bth_sdk`, `bgtp`, `bt_app`, `bth_gle`, `bts_header`
- `BT_GLE_ONLY` 宏**未定义** → 完整 BR/EDR + BLE 栈
- `hso_enable_bt: True` → HSO 分区包含 BT ROM 代码
- `libbt_host.a` = 1.1 MB → 确认非精简版

### 6.2 头文件包含路径

在 `includes/ble/CMakeLists.txt` 中添加：
```cmake
# 蓝牙 BR/EDR 头文件（SDK 已验证存在）
target_include_directories(main_app PRIVATE
    ${CMAKE_SOURCE_DIR}/include/middleware/services/bts/br
    ${CMAKE_SOURCE_DIR}/include/middleware/services/bts/common
)
```

### 6.3 链接库

**无需额外链接。** `bth_sdk` 已通过 `target_config.py` 的 `ram_component` 列表被链接到 `ws63-liteos-app`。所有 A2DP/AVRCP API 都在 `libbt_host.a` 中。

---

## 七、完整代码骨架

### `ble.hpp` 完整声明

```cpp
#pragma once

// ============================================================
// WS63 经典蓝牙 (BR/EDR) A2DP Sink 接收端
// 目标：手机蓝牙设置直连，无需 App
//
// 协议栈：
//   GAP (BR/EDR)  — 设备发现、ACL 连接、配对
//   A2DP Sink     — 音频流接收与解码 (SBC/AAC)
//   AVRCP Target  — 响应手机播放/暂停/音量控制
// ============================================================

extern "C" {
#include "bts_br_gap.h"
#include "bt_audio_hal_interface.h"
#include "bts_avrcp_target.h"
#include "bt_audio.h"
#include "errcode.h"
#include "soc_osal.h"
}

class bt {
public:
    bt();  // 构造函数：注册回调 + enable_bt_stack()

    // 外部数据注入接口（与 sle 保持相同风格）
    using data_process_t = void (*)(const int16_t *data, uint32_t length);
    using data_clear_t   = void (*)();
    static void set_data_process_function(data_process_t callback);
    static void set_data_clear_function(data_clear_t callback);

private:
    // 初始化（在 BT stack TURN_ON 回调中按序调用）
    static void setup_local_device();
    static void setup_scan_mode();
    static void setup_audio_listener();

    // GAP 回调
    static void bt_stack_state_cb(const int transport, const int status);
    static void acl_state_changed_cb(const bd_addr_t *bd_addr, gap_acl_state_t state, unsigned int reason);
    static void pair_requested_cb(const bd_addr_t *bd_addr);
    static void pair_confirmed_cb(const bd_addr_t *bd_addr, int req_type, int number);
    static void pair_status_changed_cb(const bd_addr_t *bd_addr, int status);

    // A2DP 音频流事件
    static void audio_event_cb(bt_audio_event_type type, const td_void *data,
                               int32_t size, td_void *context);

    // AVRCP 媒体按键
    static void avrcp_passthrough_cb(td_u32 key_operation, td_u32 key_value);

    // 数据回调指针
    static data_process_t data_process;
    static data_clear_t   data_clear;

    // 流句柄
    static td_pvoid current_stream_hdl;
    static bool    is_connected;

    // 配置常量
    static constexpr const char *device_name = "WS63-Speaker";
    static constexpr uint8_t local_addr[6] = {0xA1, 0x11, 0x00, 0x03, 0x26, 0x20};
    static constexpr td_u32 audio_port_id = 0;  // 默认 I2S 硬件直通
};
```

### `ble.cpp` 实现要点

实现细节参考教程第五章，**以下为深度排查后必须遵守的修正点**：

1. ⛔ **绝对不要在 pair 回调中调用 `gap_br_confirm_pair()`**（该函数不存在）
2. ✅ **`audio_event_cb` 中 `STREAM_OPENED` 事件正确绑定 `bt_attach_audio_port`**
3. ✅ **`STREAM_CLOSED` 中先 `bt_detach_audio_port` 再 `bt_stop_audio_stream`**
4. ✅ **ACL 断连后重新开启可发现模式**
5. ✅ **所有 SDK 结构体初始化使用 `= {0}`**
6. ⚠️ **回调在 BTS 线程上下文运行，不能调用阻塞函数（如 `osal_msleep`）**
7. ⚠️ **`bt_avrcp_tg_register_audio_cbk` 返回 `td_void`，无法检查返回值**

---

## 八、调试与验证清单

| 步骤 | 验证点 | 预期结果 |
|:--|:--|:--|
| 1 | 编译通过 | 无编译错误 |
| 2 | `bt_stack_state_cb` 被调用 | 日志显示 `status == TURN_ON` |
| 3 | `setup_scan_mode` 执行 | 手机蓝牙搜索可见 `WS63-Speaker` |
| 4 | 手机点击连接 | `pair_requested_cb` 被调用 |
| 5 | 配对完成 | `pair_status_changed_cb(status=PAIRED)` |
| 6 | ACL 连接建立 | `acl_state_changed_cb(CONNECTED)` |
| 7 | A2DP 流创建 | `audio_event_cb(STREAM_CREATE)` |
| 8 | A2DP 流打开 | `audio_event_cb(STREAM_OPENED)` → `bt_attach_audio_port` |
| 9 | 手机播放音乐 | `audio_event_cb(STREAM_STRAT)` → 有声音 |
| 10 | 手机暂停 | `audio_event_cb(STREAM_SUSPENDED)` → 声音停止 |
| 11 | 手机按下一首 | `avrcp_passthrough_cb(0x4B)` 被调用 |
| 12 | 断连后重新可发现 | 手机可再次搜索到设备 |

---

## 九、常见问题预案（基于深度排查修正）

| 问题 | 根因分析 | 排查/解决 |
|:--|:--|:--|
| 手机搜不到 | `gap_br_set_bt_scan_mode` 未在 `TURN_ON` 后执行，或 `enable_bt_stack()` 失败 | 检查 `bt_stack_state_cb` 是否被调用且 `status == BT_STACK_STATE_TURN_ON` |
| 搜到但无法连接 | ① SDK 默认自动接受配对 → 应能连接；② 若 `BT_GLE_ONLY` 意外启用则 BR/EDR 不可用 | 查看 `pair_requested_cb` 是否触发；若未触发说明手机没走 BR/EDR 路径；尝试注册 `is_accept_conn_on_safe_mode_callback` 返回 `true` |
| 连接成功无声音 | `share_mem_id = 0` 路由到的 I2S 端口与实际硬件不匹配 | 尝试 `share_mem_id = 1, 2, 3...`；或走手动路径注册 `data_process` 回调后调用 `iis::data_write` |
| 编译报错找不到 `gap_br_confirm_pair` | ⛔ 教程代码遗留的调用 | **删除**所有 `gap_br_confirm_pair()` 调用 |
| 编译报错找不到 `bt_audio.h` 或 `bts_br_gap.h` | CMake include 路径未配置 | 在 `includes/ble/CMakeLists.txt` 添加 `${CMAKE_SOURCE_DIR}/include/middleware/services/bts/br` 和 `.../common` |
| 链接错误 undefined symbol | `bth_sdk` 未链接（不应发生，已验证已配置） | 若确实出现，检查 `target_config.py` 中 `ram_component` 是否含 `bth_sdk` |
| AVRCP 按键无响应 | `bt_avrcp_tg_register_audio_cbk` 返回 void 无法判断是否成功 | 确认 `bt_avrcp_tg_bts_cbk` 结构体的 `notify_pass_through_status_cbk` 字段已赋值 |

---

## 十、实施顺序建议

```
第 1 步：Kconfig 配置 → 确保 BR/EDR + A2DP + AVRCP 已启用
第 2 步：修改 CMakeLists.txt 系列 → 确保编译系统正确
第 3 步：实现 ble.hpp / ble.cpp → 核心蓝牙封装
第 4 步：实现 ble_task.cpp → 任务入口
第 5 步：集成到 main.cpp → 创建 ble_task 线程
第 6 步：编译验证 → python build.py ws63-liteos-app
第 7 步：烧录测试 → 手机蓝牙搜索配对测试
第 8 步：根据测试结果修正配对确认问题
第 9 步：调试音频输出路径（share_mem_id）
```

---

*本方案基于 WS63 SDK 实际头文件验证（2026-06-13），随 SDK 版本更新可能需调整。*
