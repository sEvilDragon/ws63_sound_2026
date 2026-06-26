# TTP229 触摸驱动 + UI 状态机 功能说明

> 本文档描述 WS63 控制端通过 TTP229 触摸模块实现音响控制的全部设计意图。
> 实现代码位于 `realize/ttp229/` (驱动) 与 `realize/ui_task/` (UI 状态机)。
> 最终输出为一组 `spi_settings_t` 参数，通过 SPI 下发给接收端（SPI 链路见 `realize/spi_task/`）。

---

## 1. 硬件构成

| 项 | 值 |
|---|---|
| 模块 | HW136，芯片 TTP229-BSF（不支持 I²C，只走两线串行） |
| TP0 | 悬空 → 输出正相（按下=1） |
| TP1 | 悬空 → 多键同时有效（滑条质心/峰检测需要） |
| TP2 | 接 GND → 16 键模式 |
| SCL | GPIO00（WS63 输出） |
| SDO | GPIO01（WS63 输入） |
| 电平 | 3.3V，ACTIVE_LOW=false |

模块丝印标注的 pad 编号与芯片 pin 号**错位 1 档**：

```
模块丝印:    1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16
芯片 pin:    0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
```

驱动内部全部使用 **1-based 芯片 pin 号**，对应 `raw bit N = chip pin (N+1)`。

---

## 2. 功能总览

用户通过 8 个滑条 pad + 3 个功能键 pad 控制音响：

```
物理布局 (raw bit):
   功能键:  [4=A]  [5=B]  [6=C]                   ← 3 个独立按键
   滑条:    [9] [11] [8] [10] [14] [15] [0] [1]  ← 8 段位置滑条 (raw bit 序)
```

最终要控制的参数：

| 参数 | 范围 | 用途 |
|---|---|---|
| `volume`     | 0..100 | 主音量 |
| `brightness` | 0..100 | 灯效亮度 |
| `bass`       | 0..100 | 低音强度 |
| `mode`       | SLE / DLNA / wired | 音频输入源 |
| `hotspot`    | ON / OFF | Wi-Fi 热点 |
| `network`    | CONN / DISC | 网络连接（与 hotspot 组合编码） |

---

## 3. 双模式 UI 状态机

整个控制分为 **MAIN** 和 **CONFIG** 两种模式，按 pad A 在两者之间切换。

```
        ┌────────────────────────┐
        │  MAIN 模式 (默认)      │
        │  滑条 = 音量长按       │
        │  滑动 = 切换输入源     │
        │  pad C = hotspot 切换  │
        │  pad A = 进入 CONFIG   │
        └─────────┬──────────────┘
                  │ pad A
                  ▼
        ┌────────────────────────┐
        │  CONFIG 模式           │
        │  滑条 = 跟手调节当前   │
        │         target         │
        │  pad B = 切 target     │
        │  pad C = hotspot 切换  │
        │  pad A = 回 MAIN       │
        └────────────────────────┘
```

### 3.1 MAIN 模式行为

| 输入 | 行为 |
|------|------|
| 滑条 **快速滑动** (连续移动 ≥ 2 逻辑档位且 speed且 speed > 5) | 循环切换输入源：SLE → DLNA → wired → SLE |
| 滑条 **单指长按** 在左半侧 (逻辑 pos 0..3) | 每 100ms volume −1（带首次延迟 500ms） |
| 滑条 **单指长按** 在右半侧 (逻辑 pos 4..7) | 每 100ms volume +1（带首次延迟 500ms） |
| pad C 短按 | hotspot ON/OFF 切换 |
| pad A 短按 | 进入 CONFIG 模式，target=mode |

> **"滑动"必须是一次连续的触碰动作**。点左端 → 释放 → 点右端 不算滑动（anchor 在释放时重置）。

### 3.2 CONFIG 模式行为

> **核心交互模型：鼠标滚轮。** 滑条不映射绝对位置, 而是检测手指移动的**相对增量**累积到当前值。
> 手指从左滑到右 → value 增加; 从右滑到左 → value 减少。抬起再滑继续累加。value 有上下限 (0..100)。
> 灵敏度 `SLIDER_SENSITIVITY=50`: 一次全 8-pad 滑程 ≈ ±50 单位。

| 输入 | 行为 |
|------|------|
| pad B 短按 | 循环切 target：mode → volume → brightness → bass |
| pad C 短按 | hotspot ON/OFF 切换 |
| pad A 短按 | 返回 MAIN 模式 |
| 滑条 **触碰 / 移动** | 滚轮式增量调节：<br>• `value += (Δpos × 50) / 700`，clamp 到 0..100<br>• target=mode 时 `mode = MODE_LIST[(pos/100) % 3]` (绝对位置) |
| 滑条 **单指长按** 在左/右侧 | 每 100ms 当前 target ±1（mode 除外） |

### 3.3 target 列表

| target | 滑条滚轮 | 长按 ±1 |
|--------|---------|---------|
| mode       | (pos/100)%3 选择 SLE/DLNA/wired (绝对位置) | 禁止 |
| volume     | 相对增量: value += Δpos×50/700, clamp 0..100 | 支持 |
| brightness | 相对增量: value += Δpos×50/700, clamp 0..100 | 支持 |
| bass       | 相对增量: value += Δpos×50/700, clamp 0..100 | 支持 |

---

## 4. 滑条驱动算法

### 4.1 滑条 pad 物理→逻辑映射

芯片 pin 号数组（按物理从左到右，1-based）：

```cpp
// raw bit:        9  11   8  10  14  15   0   1
PAD_SLIDER[8] = {10, 12, 9, 11, 15, 16, 1, 2};
//  index         0   1  2   3   4   5  6   7   ← 逻辑位置 pos
```

> 功能键对应 pin: `PAD_FUNC_A=5, B=6, C=7` (raw bit 4/5/6).
> 如果发现滑动方向反了, 把整个 `PAD_SLIDER` 数组 reverse 即可.

### 4.2 位置计算：最大连续段质心 (largest-run centroid)

**不用全局质心**：全局质心会把物理上不挨着的远处误触 pad 也纳入平均，导致位置被拖偏。
例如手指在 pin 14(idx=6) 但 pin 9(idx=0) 意外桥接 → 全局质心 = (0+6)/2 = 3.00 → 明明在右边却被拉到中间。

**当前算法：找到 PAD_SLIDER 索引中连续被按下的最长一段，取其质心。非连续孤立触发被自动忽略。**

```cpp
1. 扫描 8 个 pad，标记哪些被按下
2. 找最长连续 true 的段（等长取最右段）
3. 对该段内所有 pad 的索引取平均，×100 输出
```

特性：

- 手指正常滑动激活 1~3 个相邻 pad → 正常质心插值
- 远处 pad 被意外桥接（非连续） → 被忽略，不干扰位置
- 单向 swipe 单调不回跳
- 100 倍缩放提供亚位精度
- **UI 层不将绝对位置映射到 0..100, 而是用相对增量累积（鼠标滚轮模型）**

### 4.3 方向与速度

驱动维护 `slider_direction` 和 `slider_speed`：

- 位置变化时重算 speed = (Δpos × 100) / (Δticks × 100), 归一化到逻辑档位/秒
- 位置不变时 speed 每 500 tick 减半衰减
- UI 用 `(speed > 5 && |Δpos| ≥ 200)` 判定"快速 swipe" (Δpos 为缩放值, 200 = 2 逻辑档位)

### 4.4 长按识别

UI 侧判定：

- 必须 `popcount(raw_state) == 1`（单指，排除多指过渡）
- 必须同侧连续 ≥ 500ms（100 tick × 5ms）
- 之后每 100ms（20 tick）触发一次 ±1

---

## 5. 功能键驱动

### 5.1 按键映射

```
PAD_FUNC_A = 5    // raw bit 4 → chip pin 5
PAD_FUNC_B = 6    // raw bit 5 → chip pin 6
PAD_FUNC_C = 7    // raw bit 6 → chip pin 7
```

### 5.2 消抖状态机

每键维护 `stable`、`debounce_cnt`、`hold_cnt`：

```
           raw 连续 2 帧与 stable 不同
stable=0 ─────────────────────────────► stable=1
           ▲                               │
           │                               │ hold_cnt>0 时忽略反向
           │  raw 连续 2 帧与 stable 不同  │
stable=1 ─────────────────────────────► stable=0
```

参数：

| 参数 | 值 | 含义 |
|------|----|------|
| `FUNC_DEBOUNCE_TICKS` | 2 | 连续 2 帧一致才接受（≈ 66ms @33ms 扫描） |
| `FUNC_HOLD_TICKS`     | 3 | 接受后 ~100ms 内忽略反向抖动（原 20=660ms 太长） |

### 5.3 事件产出

- `func_pressed`：当前按下状态（位掩码，bit 0=A / 1=B / 2=C）
- `func_just_pressed` / `func_just_released`：本帧边沿（**只在 ttp_task 那一帧为 1**）

为避免 5ms UI 轮询 vs 33ms TTP 轮询造成的重复消费，驱动额外维护**锁存变量**：

```cpp
uint8_t ttp_consume_press_latch(void);    // 读并清零
uint8_t ttp_consume_release_latch(void);  // 读并清零
```

UI 在每个 tick 入口调用一次，整个 tick 内复用本地副本，保证每条边沿**只被处理一次**。

---

## 6. SPI 协议对齐

`spi_settings_t` 字段（见 `realize/spi_task/spi_settings.h`）：

| 字段 | 编码 |
|------|------|
| `mode` | SPI_MODE_SLE=63 / SPI_MODE_DLNA=127 / SPI_MODE_WIREED=0 |
| `volume` / `brightness` / `bass` | 0..100，写入前 clamp |
| `hotspot_network` | `spi_make_hotspot_network(hotspot, network)` |
| `hotspot` | SPI_HOTSPOT_ON=7 / SPI_HOTSPOT_OFF=3 |
| `network` | SPI_NETWORK_CONN=3 / SPI_NETWORK_DISC=7 |

> 当前 `spi_slave_task` 在 `main.cpp` 中被注释，本地 `g_settings` 改动尚未同步到接收端。
> 启用 SPI 后即可把上述 UI 操作实时下发。

---

## 7. 日志分级

| 宏 | 含义 | 默认 |
|----|------|------|
| `TTP_LOG` | 关键事件（模式切换、target 切换、slider 位置变化、心跳、周期 dump） | 常开 |
| `TTP_VLOG` | 详细诊断（逐 bit 串行、单键消抖细节） | `TTP_VERBOSE=0` 关闭 |

需要调试 bit-bang 信号时把 `ttp_task.cpp` 顶部的 `TTP_VERBOSE` 改为 1 重新编译。

---

## 8. 验证清单

### 8.1 按键

- [ ] 短按 pad 14 → 只产生 1 行 `[UI] -> CONFIG` 或 `[UI] -> MAIN`
- [ ] 短按 pad 15（CONFIG 模式）→ 只产生 1 行 `[UI] config target -> xxx`
- [ ] 短按 pad 16 → 只产生 1 行 `[UI] hotspot -> ON/OFF`

### 8.2 滑条

- [ ] CONFIG + target=volume：手指从左滑到右，volume 增加 (不会回退)
- [ ] CONFIG + target=volume：手指从右滑到左，volume 减少
- [ ] CONFIG + target=volume：多次重复左→右滑，volume 能到达 100 (滚轮累积)
- [ ] CONFIG + target=volume：多次重复右→左滑，volume 能到达 0
- [ ] CONFIG + target=volume：手指停在某处不动，volume 不变
- [ ] MAIN：点最左 → 释放 → 点最右 → 释放，**不**触发 mode 切换
- [ ] MAIN：连续滑过 ≥ 2 逻辑档位，触发一次 mode 切换
- [ ] MAIN：长按逻辑 pos 0..3，volume 递减（带首次 500ms 延迟）
- [ ] MAIN：长按逻辑 pos 4..7，volume 递增（带首次 500ms 延迟）
- [ ] CONFIG：长按任意侧，当前 target ±1（mode 除外）

### 8.3 SPI（启用后）

- [ ] 每次 UI 修改后，接收端能在 ~50ms 内拿到新值
- [ ] mode 切换、volume 修改、hotspot 切换都正确下发

---

## 9. 已知风险 / 后续优化

1. **PAD_SLIDER pin 号可能需要调整**：功能键已固定在 pin 1/2/3, 滑条默认 pin 9..16。如果实际模块布局不同, 请对照 raw 日志自行修改 `PAD_SLIDER` 数组。
2. **灵敏度调优**：`SLIDER_SENSITIVITY=50` (一次全滑程 ±50)。觉得太快改小, 太慢改大。在 `ui_task.cpp` 顶部修改。
3. **多指过渡帧**：`popcount == 1` 过滤掉了多指同时触碰的帧，长按判定可能在手指刚落下时短暂失效（正常现象）。
4. **SPI 链路未启用**：`spi_slave_task` 在 `main.cpp` 中注释，启用后需要联调接收端。
5. **滑条 release 瞬间短暂 NO_POS**：未加最小保持窗口，UI 会看到一次 released。若 UI 侧对位置跳变敏感，可在驱动里加 1~2 帧 hold。
