# SPI 设置通讯协议使用指南

## 协议概述

SPI 设置协议是一个 **6 字节** 的定长帧，由 **接收端（Master，SPI_BUS_1）** 主动发起，每 **500ms** 与 **控制端（Slave，SPI_BUS_0）** 交换一次数据。

### 帧结构

| 字节 | 字段名              | 说明                                 |
|------|---------------------|--------------------------------------|
| 0    | cmd                 | 命令类型                             |
| 1    | hotspot_network     | 热点状态（高4位）+ 网络状态（低4位） |
| 2    | mode                | 音源模式                             |
| 3    | volume              | 音量（0-100）                        |
| 4    | brightness          | 亮度（0-100）                        |
| 5    | bass                | 低音强度（0-100）                    |

### 第0字节 - 命令类型

| 数值 | 名称  | 说明                                         |
|------|-------|----------------------------------------------|
| 0    | QUERY | 主机查询从机当前设置                          |
| 127  | SYNC  | 主机推送新设置，从机收到后更新自身状态         |

### 第1字节 - 热点与网络（高低半字节打包）

**高4位**（bit 7-4）：热点状态
| 数值 | 含义       |
|------|-----------|
| 3    | 热点关闭   |
| 7    | 热点开启   |

**低4位**（bit 3-0）：网络状态
| 数值 | 含义       |
|------|-----------|
| 3    | 网络已连接 |
| 7    | 网络已断开 |

示例：热点关闭 + 网络已连接 = `(3 << 4) | 3` = `0x33`
示例：热点开启 + 网络已断开 = `(7 << 4) | 7` = `0x77`

### 第2字节 - 模式选择

| 数值 | 十六进制 | 含义           |
|------|---------|---------------|
| 0    | 0x00    | 有线连接       |
| 63   | 0x3F    | 星闪连接（SLE）|
| 127  | 0x7F    | DLNA          |
| 85   | 0x55    | 星闪麦克风模式 |
| 170  | 0xAA    | DLNA网络连接模式|

### 第3-5字节 - 百分比值

| 字节 | 字段       | 有效范围 |
|------|-----------|---------|
| 3    | volume     | 0 - 100 |
| 4    | brightness | 0 - 100 |
| 5    | bass       | 0 - 100 |

超出 0-100 的值视为无效，整帧将被丢弃，不会更新任何设置。

---

## 文件结构

### 接收端（Master）
```
receiving end/
  main.cpp                              -- 创建 spi_master_task
  realize/spi_task/
    spi_settings.h                      -- 协议结构体、常量、校验函数
    spi_task.h                          -- 任务声明、外部修改/读取接口
    spi_task.cpp                        -- Master 端任务实现
    spi_settings_tutorial.md            -- 本文档
  includes/spi_master/
    spi_master.hpp / spi_master.cpp     -- 底层 SPI 驱动
```

### 控制端（Slave）
```
control end/
  main.cpp                              -- 创建 spi_slave_task
  realize/spi_task/
    spi_settings.h                      -- 协议结构体、常量、校验函数
    spi_task.h                          -- 任务声明、外部读取接口
    spi_task.cpp                        -- Slave 端任务实现
  includes/spi_slave/
    spi_slave.hpp / spi_slave.cpp       -- 底层 SPI 驱动
```

---

## API 参考

### 共用类型与常量（`spi_settings.h`）

```c
typedef struct {
    uint8_t cmd;                // SPI_CMD_QUERY (0) 或 SPI_CMD_SYNC (127)
    uint8_t hotspot_network;    // 高4位=热点，低4位=网络
    uint8_t mode;               // SPI_MODE_* 系列常量
    uint8_t volume;             // 0-100
    uint8_t brightness;         // 0-100
    uint8_t bass;               // 0-100
} spi_settings_t;
```

**常量：**
```c
SPI_CMD_QUERY          // 0    - 查询命令
SPI_CMD_SYNC           // 127  - 同步命令
SPI_HOTSPOT_OFF        // 3    - 热点关闭
SPI_HOTSPOT_ON         // 7    - 热点开启
SPI_NETWORK_CONN       // 3    - 网络已连接
SPI_NETWORK_DISC       // 7    - 网络已断开
SPI_MODE_WIREED        // 0    - 有线连接
SPI_MODE_SLE           // 63   - 星闪连接
SPI_MODE_DLNA          // 127  - DLNA
SPI_MODE_SLE_MIC       // 0x55 - 星闪麦克风
SPI_MODE_DLNA_NET      // 0xAA - DLNA网络连接
```

**校验函数：**
```c
int spi_validate_settings(const spi_settings_t *s);   // 校验整个帧的所有字段
int spi_validate_cmd(uint8_t v);                       // 校验命令字节
int spi_validate_mode(uint8_t v);                      // 校验模式字节
int spi_validate_percent(uint8_t v);                   // 校验百分比值（0-100）
int spi_validate_hotspot_network(uint8_t v);            // 校验热点+网络字节
```

**辅助函数：**
```c
uint8_t spi_make_hotspot_network(uint8_t hotspot, uint8_t network);  // 打包热点和网络到一个字节
uint8_t spi_get_hotspot(uint8_t v);   // 从打包字节中提取热点状态
uint8_t spi_get_network(uint8_t v);   // 从打包字节中提取网络状态
```

---

### 接收端（Master）外部接口（`spi_task.h`）

#### `void spi_settings_update_mode(uint8_t mode)`
设置新的音源模式，自动标记为 SYNC，下次 SPI 传输时推送给控制端。
```cpp
spi_settings_update_mode(SPI_MODE_SLE);       // 切换到星闪
spi_settings_update_mode(SPI_MODE_DLNA);      // 切换到 DLNA
spi_settings_update_mode(SPI_MODE_WIREED);    // 切换到有线
spi_settings_update_mode(SPI_MODE_SLE_MIC);   // 切换到星闪麦克风
spi_settings_update_mode(SPI_MODE_DLNA_NET);  // 切换到 DLNA 网络连接
```

#### `void spi_settings_update_volume(uint8_t volume)`
设置音量（0-100），自动标记为 SYNC。
```cpp
spi_settings_update_volume(75);   // 音量设为 75%
spi_settings_update_volume(0);    // 静音
```

#### `void spi_settings_update_brightness(uint8_t brightness)`
设置亮度（0-100），自动标记为 SYNC。
```cpp
spi_settings_update_brightness(50);   // 亮度设为 50%
```

#### `void spi_settings_update_bass(uint8_t bass)`
设置低音强度（0-100），自动标记为 SYNC。
```cpp
spi_settings_update_bass(80);   // 低音设为 80%
```

#### `void spi_settings_update_hotspot_network(uint8_t hotspot, uint8_t network)`
同时设置热点和网络状态，自动标记为 SYNC。
```cpp
// 开启热点，网络已连接
spi_settings_update_hotspot_network(SPI_HOTSPOT_ON, SPI_NETWORK_CONN);

// 关闭热点，网络已断开
spi_settings_update_hotspot_network(SPI_HOTSPOT_OFF, SPI_NETWORK_DISC);
```

#### `const spi_settings_t *get_spi_settings()`
获取当前设置的只读指针，可在任意任务中调用。
```cpp
const spi_settings_t *s = get_spi_settings();
osal_printk("音量=%u 模式=%u\r\n", s->volume, s->mode);
```

---

### 控制端（Slave）外部接口（`spi_task.h`）

#### `const spi_settings_t *get_spi_settings()`
获取控制端当前已应用的设置的只读指针。这是控制端上其他任务读取用户设置的**唯一推荐方式**。
```cpp
const spi_settings_t *s = get_spi_settings();

// 判断当前模式
if (s->mode == SPI_MODE_SLE) {
    // 处理星闪模式
}

// 判断热点是否开启
if (spi_get_hotspot(s->hotspot_network) == SPI_HOTSPOT_ON) {
    // 热点已开启
}

// 判断网络是否连接
if (spi_get_network(s->hotspot_network) == SPI_NETWORK_CONN) {
    // 网络已连接
}

// 读取百分比值
uint8_t vol = s->volume;       // 0-100
uint8_t bri = s->brightness;   // 0-100
uint8_t bass = s->bass;        // 0-100
```

---

## 通讯流程

### SYNC 流程（主机推送新设置）
```
Master 发送:  cmd=127, hn=0x33, mode=63, vol=75, bri=50, bass=80
Slave 收到:   逐字段校验 → 全部通过 → 更新自身 g_settings
Slave 回复:   当前设置（cmd=0）
Master 收到:  校验回复 → 记录日志
Master 下一步: 将 cmd 切回 0（下次传输变为查询模式）
```

### QUERY 流程（主机查询当前设置）
```
Master 发送:  cmd=0, hn=..., mode=..., vol=..., bri=..., bass=...
Slave 收到:   忽略 Master 发的数据，回复自己的当前设置（cmd=0）
Master 收到:  校验回复 → 用 Slave 的回复更新本地副本
```

### 典型工作周期
1. 外部代码在接收端调用 `spi_settings_update_volume(80)`
2. 内部将 `cmd` 设为 `127`（SYNC），`volume` 设为 `80`
3. 下一次 SPI 传输发送 `cmd=127` 和新的音量值
4. 控制端收到后校验通过，更新自身设置，回复当前状态
5. 接收端确认成功后，将 `cmd` 切回 `0`（QUERY）
6. 后续传输都是查询模式，直到再次调用 update 函数

---

## 使用示例

### 示例1：WiFi 任务在接收端切换模式
```cpp
#include "spi_task.h"

// 在 WiFi 任务中，当用户通过 DLNA 连接时：
spi_settings_update_mode(SPI_MODE_DLNA);

// 之后切换回有线输入：
spi_settings_update_mode(SPI_MODE_WIREED);
```

### 示例2：LED 任务在控制端读取亮度
```cpp
#include "spi_task.h"

void *led_control_task(void *arg) {
    while (true) {
        const spi_settings_t *s = get_spi_settings();
        uint8_t brightness = s->brightness;
        // 根据 brightness 控制 LED 亮度...
        osal_msleep(100);
    }
}
```

### 示例3：音频任务在控制端读取音量和模式
```cpp
#include "spi_task.h"

void *audio_control_task(void *arg) {
    while (true) {
        const spi_settings_t *s = get_spi_settings();

        switch (s->mode) {
            case SPI_MODE_WIREED:   /* 有线输入 */  break;
            case SPI_MODE_SLE:      /* 星闪输入 */  break;
            case SPI_MODE_DLNA:     /* DLNA 输入 */ break;
            case SPI_MODE_SLE_MIC:  /* 星闪麦克风 */ break;
            case SPI_MODE_DLNA_NET: /* DLNA 网络 */  break;
        }

        set_volume(s->volume);
        set_bass(s->bass);

        osal_msleep(100);
    }
}
```

### 示例4：在接收端更新网络状态
```cpp
#include "spi_task.h"

// WiFi 连接成功时：
spi_settings_update_hotspot_network(SPI_HOTSPOT_OFF, SPI_NETWORK_CONN);

// 启动配网热点时：
spi_settings_update_hotspot_network(SPI_HOTSPOT_ON, SPI_NETWORK_DISC);
```

---

## 校验规则

每个字段独立校验，**任何一个字段不合法，整帧都会被丢弃**，保持原有设置不变。

| 字段                | 合法值                          |
|---------------------|--------------------------------|
| cmd                 | 0 或 127                       |
| hotspot（高4位）    | 3 或 7                         |
| network（低4位）    | 3 或 7                         |
| mode                | 0、63、127、0x55、0xAA         |
| volume              | 0 - 100                        |
| brightness          | 0 - 100                        |
| bass                | 0 - 100                        |

### 为什么选择 3 和 7？

数值 3（二进制 `0011`）和 7（二进制 `0111`）之间**汉明距离较大**，如果传输中发生单 bit 翻转，可以直接被检测出来并丢弃。同理，模式值（0、63、127、85、170）在整个 uint8_t 范围内分布较远，不容易因小幅度干扰而混淆。
