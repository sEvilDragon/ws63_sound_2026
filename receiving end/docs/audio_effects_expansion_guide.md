# 音频效果链扩展指南

## 当前已有的处理链

```
原始PCM → 音量缩放(查表乘法) → 低音搁架滤波(biquad) → DMA缓冲区
```

对应参数：volume (0-100), bass (0-100)

## 扩展目标：5段EQ + 效果链

```
原始PCM → 音量 → EQ段1(60Hz) → EQ段2(250Hz) → EQ段3(1kHz) → EQ段4(4kHz) → EQ段5(12kHz) → 低音 → DMA
```

---

## 第一部分：5段EQ实现

### 1.1 滤波器类型分配

| 段 | 频率 | 滤波器类型 | RBJ公式 |
|----|------|-----------|---------|
| 1 | 60Hz | Low Shelf（低频搁架） | 和现有bass相同 |
| 2 | 250Hz | Peaking EQ（峰值均衡） | 见下方 |
| 3 | 1kHz | Peaking EQ | 见下方 |
| 4 | 4kHz | Peaking EQ | 见下方 |
| 5 | 12kHz | High Shelf（高频搁架） | 见下方 |

### 1.2 Peaking EQ 系数公式（RBJ Audio Cookbook）

```cpp
// Peaking EQ: 只影响中心频率附近
float A  = powf(10.0f, dbGain / 40.0f);  // 注意除以40不是20
float w0 = 2.0f * PI * freq / sampleRate;
float cs = cosf(w0);
float sn = sinf(w0);
float al = sn / (2.0f * Q);               // Q控制带宽，典型值1.0-2.0

float a0 = 1.0f + al / A;
float b0 = (1.0f + al * A) / a0;
float b1 = (-2.0f * cs) / a0;
float b2 = (1.0f - al * A) / a0;
float a1 = b1;                             // peaking EQ的a1等于b1
float a2 = (1.0f - al / A) / a0;
```

### 1.3 High Shelf 系数公式

```cpp
// High Shelf: 增强/衰减高频
float A  = powf(10.0f, dbGain / 40.0f);
float w0 = 2.0f * PI * freq / sampleRate;
float cs = cosf(w0);
float sn = sinf(w0);
float al = sn / 2.0f * sqrtf((A + 1.0f/A) * (1.0f/Q - 1.0f) + 2.0f);

float a0 = (A+1) - (A-1)*cs + 2*sqrtf(A)*al;
float b0 = (A * ((A+1) + (A-1)*cs + 2*sqrtf(A)*al)) / a0;
float b1 = (-2*A * ((A-1) + (A+1)*cs)) / a0;
float b2 = (A * ((A+1) + (A-1)*cs - 2*sqrtf(A)*al)) / a0;
float a1 = (2 * ((A-1) - (A+1)*cs)) / a0;
float a2 = ((A+1) - (A-1)*cs - 2*sqrtf(A)*al) / a0;
```

### 1.4 建议的数据结构

```cpp
// 单个biquad滤波器
struct biquad_filter {
    float b0, b1, b2, a1, a2;    // 系数
    float x1_l, x2_l, y1_l, y2_l;  // 左声道状态
    float x1_r, x2_r, y1_r, y2_r;  // 右声道状态
};

// EQ预设
struct eq_preset {
    int8_t gain_db[5];  // 5段各自的dB增益，范围 -12 ~ +12
};

// 预设表
static const eq_preset presets[] = {
    { {0,  0,  0,  0,  0} },  // 平坦（关闭）
    { {3,  1, -1,  2,  3} },  // 流行
    { {4,  2, -1,  3,  4} },  // 摇滚
    { {2,  1,  0,  1,  3} },  // 古典
    { {-2, -1, 3,  3,  1} },  // 人声
    { {2,  3,  1,  2,  2} },  // 爵士
};
```

### 1.5 在 data_write 中集成

```cpp
// 在现有 bass biquad 之后，串联5个EQ biquad
for (int band = 0; band < 5; band++) {
    if (eq_gains[band] != 0) {  // 跳过增益为0的段
        // 用预计算的系数处理每个样本（和bass处理逻辑相同）
        // ...
    }
}
```

---

## 第二部分：SPI协议扩展

### 2.1 当前协议（6字节）

```
[cmd] [hotspot|network] [mode] [volume] [brightness] [bass]
```

### 2.2 扩展方案A：增加字节（简单但需要改传输长度）

```
[cmd] [hotspot|network] [mode] [volume] [brightness] [bass] [eq_preset] [treble]
```

- Byte 6: EQ预设索引（0=平坦, 1=流行, 2=摇滚, 3=古典, 4=人声, 5=爵士）
- Byte 7: 高音 (0-100)，和bass对称

传输长度从 16 字节仍然够用（当前只用6字节，剩余10字节都是0）。

### 2.3 扩展方案B：复用mode字段的高位

如果不想增加字节数，可以在mode字段中编码更多信息。但建议用方案A，清晰且不影响现有逻辑。

### 2.4 spi_settings.h 需要添加

```c
#define SPI_EQ_FLAT    0
#define SPI_EQ_POP     1
#define SPI_EQ_ROCK    2
#define SPI_EQ_CLASSIC 3
#define SPI_EQ_VOCAL   4
#define SPI_EQ_JAZZ    5

typedef struct {
    uint8_t cmd;
    uint8_t hotspot_network;
    uint8_t mode;
    uint8_t volume;
    uint8_t brightness;
    uint8_t bass;
    uint8_t eq_preset;   // 新增
    uint8_t treble;      // 新增
} spi_settings_t;

// SPI_SETTINGS_LEN 从 6 改为 8
#define SPI_SETTINGS_LEN  8
```

---

## 第三部分：其他可扩展的音效

### 3.1 响度补偿（Loudness）

**原理：** 人耳在小音量时对低频和高频的敏感度下降（等响曲线）。低音量时自动加bass和treble。

**实现：**
```cpp
// 在volume处理后，根据音量值自动调整bass和treble增益
float loudness_bass_boost = 0;
float loudness_treble_boost = 0;
if (volume < 50) {
    // 音量越低，补偿越多
    float factor = (50 - volume) / 50.0f;  // 0.0 ~ 1.0
    loudness_bass_boost = factor * 6.0f;    // 最多+6dB
    loudness_treble_boost = factor * 4.0f;  // 最多+4dB
}
```

**不需要额外SPI字段**，纯软件逻辑，根据当前volume自动计算。

### 3.2 立体声加宽（Stereo Widening）

**原理：** 将一部分左声道信号混入右声道（反相），反之亦然，制造更宽的声场。

**实现（极其简单）：**
```cpp
float width = 0.3f;  // 加宽程度，0=无，0.5=最大
float mid = (left + right) * 0.5f;    // 中间信号
float side = (left - right) * 0.5f;   // 侧边信号
side *= (1.0f + width);                // 增强侧边
left  = mid + side;
right = mid - side;
```

**计算量：** 每个立体声样本对 4 次加法 + 2 次乘法。几乎无开销。

### 3.3 动态压缩（Dynamic Compression）

**原理：** 自动检测音量峰值并降低增益，防止爆音，保护喇叭。

**实现：**
```cpp
static float compressor_gain = 1.0f;
float peak = fabsf(sample);
float threshold = 28000.0f;  // 压缩阈值

if (peak > threshold) {
    float ratio = 4.0f;  // 4:1压缩比
    float excess = peak - threshold;
    float compressed = threshold + excess / ratio;
    float target_gain = compressed / peak;
    // 平滑过渡（attack/release）
    compressor_gain += (target_gain - compressor_gain) * 0.01f;
} else {
    compressor_gain += (1.0f - compressor_gain) * 0.001f;  // 缓慢恢复
}
sample *= compressor_gain;
```

---

## 第四部分：推荐的实现顺序

| 优先级 | 功能 | 工作量 | 效果 |
|--------|------|--------|------|
| 1 | 高音控制（High Shelf） | 小 — 复制bass代码改频率 | 音色可调 |
| 2 | EQ预设 | 中 — 5个biquad串联 | 风格切换 |
| 3 | 响度补偿 | 小 — 纯软件逻辑 | 小音量体验提升 |
| 4 | 立体声加宽 | 小 — 几行代码 | 声场变宽 |
| 5 | 动态压缩 | 中 — 需要attack/release调参 | 保护喇叭 |

---

## 第五部分：代码改动清单

以添加"高音 + EQ预设"为例：

### 需要修改的文件

| 文件 | 改动内容 |
|------|---------|
| `spi_settings.h`（两端） | 添加 eq_preset, treble 字段和校验 |
| `spi_task.h`（master） | 添加 `spi_settings_update_eq_preset()`, `spi_settings_update_treble()` |
| `spi_task.cpp`（master） | 实现新 update 函数 |
| `spi_task.cpp`（slave） | SYNC 时更新新字段 |
| `iis.hpp` | data_write 添加 treble, eq_preset 参数；添加 EQ biquad 状态变量 |
| `iis.cpp` | 添加 Peaking EQ / High Shelf 系数计算；在 data_write 中串联滤波 |
| `audio_play.cpp` | data_write 调用传入新参数 |
| `wifi_task.cpp` | data_write 调用传入新参数 |

### 性能评估

| 处理 | 每样本运算 | 960样本/buffer |
|------|-----------|---------------|
| 音量缩放 | 1次乘法 | 960次 |
| Bass biquad | 5次乘法+4次加法 | 9600次 |
| 5段EQ | 25次乘法+20次加法 | 43200次 |
| **总计** | **~31次/样本** | **~54720次** |

WS63 Cortex-M33 @ 200MHz 有 FPU，单次浮点乘加约 2-3 周期。54720 次运算 ≈ **约 0.8ms**，远小于 buffer 播放时间（960/44100 ≈ 22ms），**完全没有性能问题**。
