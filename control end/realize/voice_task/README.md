# 控制端语音指令映射

语音模块为 I2C 从机，地址为 `0x34`。本模块同时支持：

- 主动识别：控制端从结果寄存器 `0x64` 读取 `AA 55 00 ID FB` 中的 `ID`。
- 被动播报：控制端向播报寄存器 `0x6E` 写入 `0xFF + ID`，对应逻辑帧 `AA 55 FF ID FB`。

## 主动识别指令（类型 `00`）

| ID | 语音命令 | 控制端动作 |
|---|---|---|
| `01` | 切换星闪模式 | `mode = SPI_MODE_SLE` |
| `02` | 切换网络模式 | `mode = SPI_MODE_DLNA` |
| `03` / `05` | 打开 / 关闭网络连接 | 更新 `network`；打开网络时关闭热点 |
| `04` / `06` | 打开 / 关闭热点连接 | 更新 `hotspot` |
| `07` / `08` | 增大 / 减小亮度 | `brightness ± 10`，限制到 `0..100` |
| `09` | 亮度最大（表中两个命令词共用此 ID） | `brightness = 100` |
| `0A` / `0B` | 增大 / 减小音响音量 | `volume ± 5`，限制到 `0..100` |
| `0C` / `0D` | 音响音量最大 / 最小 | `volume = 100 / 0` |
| `0E` / `0F` | 开启 / 关闭夜间模式 | 设置 / 清除 `SPI_FLAG_NIGHT` |
| `10` / `11` | 增大 / 减小低音 | `bass ± 5`，限制到 `0..100` |
| `12` / `13` | 低音最大 / 最小 | `bass = 100 / 0` |
| `14` | 默认音效 | `tone = SPI_TONE_FLAT` |
| `15` | 人声音效 | `tone = SPI_TONE_VOCAL` |
| `16` | 低音增强音效 | `tone = SPI_TONE_BASS_BOOST` |
| `17` | 流行音效 | `tone = SPI_TONE_POP` |
| `18` | 摇滚音效 | `tone = SPI_TONE_ROCK` |
| `19` | 介绍自己 | 仅识别；回复由语音模块自身播报 |
| `66` | 亮度最小 | `brightness = 0` |

所有设置类指令都调用 `spi_settings_update_*()`，由控制端 DWS 从机任务同步到接收端。
接收端据此切换音频/Wi-Fi 功能并应用音量、低音、夜间和音效参数；亮度和夜间模式也会影响控制端 SK9822 灯效。

## 被动播报接口（类型 `FF`）

| ID | 播报语 | 专用接口 |
|---|---|---|
| `1A` | 模式选择 | `voice_request_mode_selection_prompt()` |
| `1B` | 音量调节 | `voice_request_volume_adjust_prompt()` |
| `1C` | 亮度调节 | `voice_request_brightness_adjust_prompt()` |
| `1D` | 打开网络 | `voice_request_network_on_prompt()` |
| `1E` | 关闭网络 | `voice_request_network_off_prompt()` |
| `64` | 打开热点 | `voice_request_hotspot_on_prompt()` |
| `65` | 关闭热点 | `voice_request_hotspot_off_prompt()` |

也可以调用通用接口：

```cpp
voice_request_prompt(VOICE_PROMPT_MODE_SELECTION);
```

接口采用异步队列：返回 `true` 表示请求成功入队，`voice_task` 随后独占 I2C 总线并向 `0x6E` 写入。
实际发送成功或失败会输出 `[VOICE] passive prompt ...` 日志。队列容量为 8，队列满或 ID 非法时返回 `false`。
