# WS63 音响功能增强实施路线图

本文档用于把后续功能拆成可以逐一实现的小任务。目标是在不显著增加接收端内存压力、不重写现有通信链路的前提下，提高音响产品完整度和演示效果。

## 当前判断

短期不建议优先做 OTA、网络麦克风、完整 5 段 EQ 这类高风险功能。当前工程已经有比较清晰的基础链路：

- 控制端通过 `spi_settings_t` 同步模式、音量、亮度、低音等设置。
- 接收端 `audio_play_task` 根据模式启动 SLE 音频或 DLNA 播放。
- SLE 音频已经是原始 PCM 透传链路，`SPI_MODE_SLE_MIC` 已被接收端当作 SLE 播放模式处理。
- 控制端已有 TTP229 触摸 UI 和 SK9822 灯效任务。
- 接收端已有 HTTP 控制入口，适合给小程序做高级控制面板。
- 发送端已有模拟声卡采集 PCM 并通过 SLE 发送的链路。

因此推荐短期功能组合：

1. 音色预设和高音控制。
2. SLE 麦克风模式包装。
3. 夜间模式。
4. 静音和音量淡入淡出。
5. 语音模块命令映射。
6. 灯效状态联动。
7. 小程序高级控制面板。

## 总体架构原则

### 统一控制面

所有控制来源都应该写入同一份状态，不要各自直接控制底层任务。

```text
TTP / 小程序 HTTP / 语音模块
        |
        v
spi_settings_t 或控制端扩展状态
        |
        v
控制端 SPI/DWS 同步
        |
        v
接收端执行音频、网络、灯效等动作
```

### TTP 和小程序分工

TTP 只保留基础控制：

- 音量。
- 亮度。
- 低音。
- 基础输入源切换。
- 热点开关。

小程序负责高级控制：

- SLE 麦克风模式。
- 网络麦克风预留。
- 音色预设。
- 夜间模式。
- 灯效模式。
- 语音模块开关或语音状态。
- 设备状态显示。

### 低内存策略

- 优先复用现有 PCM、SLE、HTTP、SPI 任务。
- 避免新增大缓冲。
- 音效优先用简单 biquad、增益、状态机。
- 网络麦克风先只预留模式，不先实现 UDP 音频。
- OTA 暂时不进入短期主线。

## 功能 1：音色预设和高音控制

### 目标

让音响具备可演示的音色风格：

- 原声 Flat。
- 人声 Vocal。
- 低音增强 Bass Boost。
- 流行 Pop。
- 夜间 Night。

可选扩展：

- 摇滚 Rock。
- 古典 Classic。

### 推荐协议扩展

当前设置包：

```text
[cmd] [hotspot|network] [mode] [volume] [brightness] [bass]
```

扩展为：

```text
[cmd] [hotspot|network] [mode] [volume] [brightness] [bass] [tone] [treble]
```

需要把两端 `SPI_SETTINGS_LEN` 从 6 改成 8。

建议新增定义：

```c
#define SPI_TONE_FLAT        0
#define SPI_TONE_VOCAL       1
#define SPI_TONE_BASS_BOOST  2
#define SPI_TONE_POP         3
#define SPI_TONE_NIGHT       4
#define SPI_TONE_ROCK        5

typedef struct {
    uint8_t cmd;
    uint8_t hotspot_network;
    uint8_t mode;
    uint8_t volume;
    uint8_t brightness;
    uint8_t bass;
    uint8_t tone;
    uint8_t treble;
} spi_settings_t;
```

### 实现策略

先不要做完整 5 段 EQ。建议做以下轻量处理：

- `bass`：沿用现有低音搁架滤波。
- `treble`：增加一个高音搁架滤波。
- `tone`：映射到 bass、treble、声场、压缩等参数组合。
- `night`：限制最大音量、降低低音、降低灯光亮度。

示例预设映射：

| 预设 | bass 修正 | treble 修正 | 声场 | 说明 |
| --- | ---: | ---: | --- | --- |
| Flat | 0 | 0 | 关闭 | 原声 |
| Vocal | -10 | +10 | 关闭 | 人声清晰 |
| Bass Boost | +25 | 0 | 关闭 | 重低音 |
| Pop | +10 | +15 | 轻微 | 流行 |
| Night | -20 | -10 | 关闭 | 夜间低扰动 |
| Rock | +20 | +15 | 轻微 | 动态更强 |

### 涉及文件

- `control end/realize/spi_task/spi_settings.h`
- `receiving end/realize/spi_task/spi_settings.h`
- `control end/realize/spi_task/spi_task.cpp`
- `receiving end/realize/spi_task/spi_task.cpp`
- `control end/realize/ui_task/ui_task.hpp`
- `control end/realize/ui_task/ui_task.cpp`
- `receiving end/includes/iis/iis.hpp`
- `receiving end/includes/iis/iis.cpp`
- `receiving end/realize/audio_play/audio_play.cpp`
- `receiving end/realize/wifi/wifi_task.cpp`
- `receiving end/realize/wifi/http_control.cpp`

### 验收标准

- 旧设置读取失败时能回落到默认设置。
- 两端设置结构长度一致。
- HTTP 状态能返回当前 `tone` 和 `treble`。
- TTP 可选：只调低音，不一定调音色。
- 小程序或 HTTP 能切换音色。
- 播放 SLE 和 DLNA 时音色都生效。

## 功能 2：SLE 麦克风模式包装

### 目标

把现有 `SPI_MODE_SLE_MIC` 做成产品功能，而不是隐藏模式。

推荐产品表达：

```text
无线麦克风模式：手机或外部音频通过模拟声卡进入发送端，再由星闪传到音响播放。
```

### 当前基础

接收端已经在 `audio_play_task` 中把 `SPI_MODE_SLE` 和 `SPI_MODE_SLE_MIC` 放在同一条 SLE 接收链路。发送端已经从 PCM2706/I2S RX 采集 PCM 并通过 SLE 发送。

### 实现策略

- 控制端 TTP 的基础模式列表暂时仍保留 `SLE / DLNA / WIRED`，不要把 TTP 操作复杂化。
- 小程序或 HTTP 增加 `SLE_MIC` 模式入口。
- 接收端状态接口显示 `SLE_MIC`。
- 控制端灯效对 `SLE_MIC` 使用不同颜色或动画，和普通 SLE 区分。

### 涉及文件

- `receiving end/realize/audio_play/audio_play.cpp`
- `receiving end/realize/wifi/http_control.cpp`
- `control end/realize/sk9822_task/sk9822_task.cpp`
- `control end/realize/spi_task/spi_settings.h`
- `receiving end/realize/spi_task/spi_settings.h`

### 可选增强

- `SLE_MIC` 模式下默认打开 Vocal 音色。
- `SLE_MIC` 模式下灯效使用麦克风状态动画。
- `SLE_MIC` 模式下禁用 DLNA 相关任务，避免资源争用。

### 验收标准

- HTTP 可以切到 `SLE_MIC`。
- 接收端切到 `SLE_MIC` 后启动 SLE 接收。
- 发送端模拟声卡输入可以被播放。
- 灯效能区分普通 SLE 与 SLE 麦克风。

## 功能 3：夜间模式

### 目标

提供一个一键模式，适合演示“智能音响”的产品化体验。

夜间模式不是新的输入源，而是一组设置：

- 音量上限 25 或 30。
- 低音降低。
- 高音略降。
- 亮度降低。
- 可选启用轻微动态压缩。

### 控制方式

- 小程序高级按钮。
- 语音命令：“打开夜间模式 / 关闭夜间模式”。
- TTP 不建议直接加入，避免基础交互变复杂。

### 协议选择

如果短期只需要演示，可以不新增字段，直接由控制端或 HTTP 一次性修改：

- `volume`
- `brightness`
- `bass`
- `tone`
- `treble`

如果后续需要保持状态，建议新增 `flags` 字段：

```c
#define SPI_FLAG_MUTE       0x01
#define SPI_FLAG_NIGHT      0x02
#define SPI_FLAG_LOUDNESS   0x04
```

### 涉及文件

- `receiving end/realize/wifi/http_control.cpp`
- `control end/realize/spi_task/spi_task.cpp`
- `receiving end/realize/spi_task/spi_task.cpp`
- `control end/realize/sk9822_task/sk9822_task.cpp`
- `receiving end/includes/iis/iis.cpp`

### 验收标准

- 进入夜间模式后音量和灯光明显下降。
- 退出夜间模式后可恢复进入前设置，或恢复默认设置。
- 状态接口能返回夜间模式状态。

## 功能 4：静音和音量淡入淡出

### 目标

提升模式切换和播放体验，减少爆音和突变。

### 静音

推荐新增状态：

- `mute = true` 时实际播放音量为 0。
- 保留用户原音量，取消静音后恢复。

如果不扩字段，可以先约定 `volume=0` 为静音；但产品上更推荐独立 `mute`。

### 淡入淡出

在 `iis::data_write()` 中引入平滑增益：

```text
current_gain -> target_gain
```

每个 buffer 更新一点，避免突然跳变。

触发场景：

- 模式切换。
- 静音/取消静音。
- 播放启动。
- SLE 连接恢复。

### 涉及文件

- `receiving end/includes/iis/iis.hpp`
- `receiving end/includes/iis/iis.cpp`
- `receiving end/realize/audio_play/audio_play.cpp`
- `receiving end/realize/wifi/wifi_task.cpp`
- `receiving end/realize/wifi/http_control.cpp`

### 验收标准

- 切模式没有明显爆音。
- 静音/恢复不会突然冲击。
- 淡入淡出不影响持续播放稳定性。

## 功能 5：语音模块命令映射

### 目标

把语音模块接入统一控制状态，让语音命令成为 TTP 和小程序之外的第三个控制入口。

### 推荐命令

第一阶段只做离散命令：

- 音量增大。
- 音量减小。
- 静音。
- 取消静音。
- 切换到星闪。
- 切换到 DLNA。
- 切换到有线。
- 打开无线麦克风。
- 打开低音增强。
- 打开人声模式。
- 打开夜间模式。
- 关闭夜间模式。
- 开启热点。
- 关闭热点。

### 接入原则

语音模块不要直接调用音频、WiFi、灯效底层接口。它只应该调用设置更新函数：

```text
voice command -> update settings -> nv_mark_dirty -> SPI sync -> receiving end applies
```

### 推荐模块结构

```text
control end/realize/voice_task/
    voice_task.cpp
    voice_task.h
    CMakeLists.txt
```

如果语音模块走 UART，还需要：

- 初始化 UART。
- 读取命令 ID。
- 做去抖和重复命令限流。

### 命令 ID 建议

```c
#define VOICE_CMD_VOL_UP       0x01
#define VOICE_CMD_VOL_DOWN     0x02
#define VOICE_CMD_MUTE         0x03
#define VOICE_CMD_UNMUTE       0x04
#define VOICE_CMD_MODE_SLE     0x10
#define VOICE_CMD_MODE_DLNA    0x11
#define VOICE_CMD_MODE_WIRED   0x12
#define VOICE_CMD_MODE_MIC     0x13
#define VOICE_CMD_TONE_FLAT    0x20
#define VOICE_CMD_TONE_VOCAL   0x21
#define VOICE_CMD_TONE_BASS    0x22
#define VOICE_CMD_NIGHT_ON     0x30
#define VOICE_CMD_NIGHT_OFF    0x31
```

### 涉及文件

- `control end/CMakeLists.txt`
- `control end/main.cpp`
- `control end/realize/voice_task/*`
- `control end/realize/spi_task/spi_task.cpp`
- `control end/realize/spi_task/spi_task.h`
- `control end/realize/spi_task/spi_settings.h`

### 验收标准

- 语音命令能稳定修改设置。
- 设置能同步到接收端。
- 语音命令和 TTP 同时操作时不会互相覆盖异常。
- NV 能保存语音修改后的最终状态。

## 功能 6：灯效状态联动

### 目标

让灯效表达当前模式和状态，增强演示感。

### 推荐灯效

| 状态 | 灯效 |
| --- | --- |
| SLE 普通播放 | 绿色频谱 |
| SLE 麦克风 | 青色或绿色呼吸 + 频谱 |
| DLNA | 红色或橙色频谱 |
| 有线 | 白色低亮 |
| 静音 | 红色短闪 |
| 夜间 | 低亮暖色 |
| 热点开启 | 蓝色扫描 |
| 模式切换 | 当前已有 wave overlay，可继续扩展 |

### 实现策略

优先复用 `sk9822_task.cpp` 的 overlay 机制，不要新增大型动画系统。

### 涉及文件

- `control end/realize/sk9822_task/sk9822_task.cpp`
- `control end/realize/spi_task/spi_settings.h`
- `control end/realize/spi_task/spi_task.cpp`

### 验收标准

- 不同模式肉眼能区分。
- 音量变化 overlay 不被长期状态覆盖。
- 夜间模式亮度明显降低。

## 功能 7：小程序高级控制面板

### 目标

小程序只做控制和状态，不做实时麦克风音频。

### 推荐页面

首页：

- 当前模式。
- 当前音量。
- 当前音色。
- 网络状态。
- 热点状态。

高级控制：

- 输入源选择。
- 无线麦克风。
- 音色预设。
- 夜间模式。
- 灯效模式。
- 静音。

### 接收端 HTTP API 建议

已有状态接口可继续扩展：

```http
GET /api/v1/status
```

建议返回：

```json
{
  "mode": "SLE_MIC",
  "volume": 25,
  "brightness": 50,
  "bass": 20,
  "treble": 50,
  "tone": "VOCAL",
  "mute": false,
  "night": false,
  "hotspot": false,
  "network": true
}
```

新增或扩展设置接口：

```http
POST /api/v1/settings
Content-Type: application/json

{
  "mode": "SLE_MIC",
  "volume": 30,
  "tone": "VOCAL",
  "night": true
}
```

### 注意事项

- HTTP 处理 buffer 当前较小，不要把请求 JSON 做得很大。
- 每次只处理几个字段。
- 非法字段忽略或返回错误。
- 设置修改后要调用 NV dirty 标记。

### 涉及文件

- `receiving end/realize/wifi/http_control.cpp`
- `receiving end/realize/wifi/http_control.hpp`
- `receiving end/realize/spi_task/spi_task.cpp`
- `receiving end/realize/spi_task/spi_task.h`

### 验收标准

- 小程序能读取状态。
- 小程序能切模式。
- 小程序能切音色。
- 小程序能打开夜间模式。
- 接收端重启后核心设置能恢复。

## 功能 8：输入源记忆和自动回退

### 目标

提升稳定性和成品感。

### 推荐行为

- 上电恢复上次模式、音量、亮度、音色。
- SLE 断开时灯效提示等待连接。
- DLNA 停止或网络断开时自动回到 SLE 或有线。
- 如果进入网络高级模式失败，回退到上一个可用模式。

### 实现策略

第一阶段只做上电恢复和失败回退，不做复杂自动检测。

### 涉及文件

- `control end/realize/spi_task/spi_task.cpp`
- `receiving end/realize/spi_task/spi_task.cpp`
- `control end/realize/sk9822_task/sk9822_task.cpp`
- `receiving end/realize/audio_play/audio_play.cpp`
- `receiving end/realize/wifi/wifi_task.cpp`

### 验收标准

- 重启后音量、模式、音色不丢。
- 网络失败不会卡在不可用状态。

## 推荐实施顺序

### 第 1 轮：协议和状态扩展

目标：

- 扩展 `spi_settings_t`。
- 增加 `tone`、`treble`。
- 保证控制端和接收端同步。
- HTTP 状态能显示新字段。

验证：

- 编译通过。
- SPI 设置同步无错。
- NV 读写兼容旧数据。

### 第 2 轮：音效生效

目标：

- `iis::data_write()` 支持 treble。
- 增加 tone 预设映射。
- SLE 和 DLNA 播放都能听出变化。

验证：

- Flat 与 Bass Boost 差异明显。
- Vocal 人声更清楚。
- Night 音量和低频更收敛。

### 第 3 轮：SLE 麦克风模式产品化

目标：

- HTTP 能切 `SLE_MIC`。
- 灯效区分 `SLE` 和 `SLE_MIC`。
- 状态接口返回 `SLE_MIC`。

验证：

- 发送端模拟声卡输入可播放。
- 切回其他模式正常。

### 第 4 轮：夜间模式和静音

目标：

- 小程序/HTTP 可开关夜间模式。
- 支持静音。
- 播放增益平滑切换。

验证：

- 切换不爆音。
- 状态显示正确。

### 第 5 轮：语音模块

目标：

- 添加 `voice_task`。
- 解析语音模块命令。
- 映射到统一设置。

验证：

- 语音切模式。
- 语音调音量。
- 语音切音色。
- 语音打开夜间模式。

### 第 6 轮：小程序整合

目标：

- 高级控制面板。
- 状态展示。
- 模式、音色、夜间、静音控制。

验证：

- 小程序和 TTP 同时操作不会冲突。
- 状态刷新准确。

## 风险点

### SPI 设置结构兼容

两端 `spi_settings_t` 必须同时更新，否则会出现字段错位。建议修改时同时更新：

- 控制端头文件。
- 接收端头文件。
- 默认值。
- 校验函数。
- NV 读取兼容逻辑。
- SPI 发送长度。

### NV 兼容

旧版本 NV 中保存的是 6 字节结构，新版本是 8 字节结构。读取时应允许旧长度失败后使用默认值，或做版本化结构。

### 音效计算量

高音和低音各一个 biquad 基本可控。完整 5 段 EQ 不建议第一阶段做。

### 小程序麦克风

小程序不建议承担实时麦克风音频链路。它可以做控制面板，真正麦克风音频应走现有模拟声卡 + SLE 发送端。

### TTP 复杂度

不要把所有高级功能塞进 TTP。TTP 操作越复杂，现场演示越容易误触。

## 新对话实施提示词

后续新对话可以按下面格式开始：

```text
请按照 receiving end/docs/product_feature_roadmap.md 的第 X 轮实现功能。
先读取 AGENTS.md 和该路线图，只做本轮范围内的改动。
保持控制端和接收端 spi_settings_t 对齐，注意 NV 兼容和构建验证。
```

建议每轮完成后记录：

- 修改了哪些文件。
- 新增了哪些协议字段。
- 如何验证。
- 还有哪些风险没有处理。

