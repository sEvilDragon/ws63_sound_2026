# MP3解码与DLNA网络流媒体播放集成指南

基于你项目中已完全实现并高度成熟的 `iis` DMA 环形缓冲队列机制（`iis::data_write`），处理 DLNA 网络音频流（HTTP GET/Push 模式）可以变得非常简单高效。

在 DLNA 这种纯连续网络数据流推送场景下，由于没有“跨文件随机快进（Seek）”的需求，我们**强烈建议采用直接循环调用 `minimp3.h` 的 Push（推流）模型**。这样可以彻底屏蔽文件 I/O，并利用你现有的 `iis` 队列来吸收网络抖动。

---

## 1. 整体架构思路

DLNA 接收端的工作流是一个典型的 **“水管模型”**：
1. **网络层 (LWIP / Socket)**：不断接收变长的 MP3 TCP 数据块（例如每次 1KB - 2KB 不等）。
2. **解码拼装层 (minimp3)**：因为网络接收到的块可能刚好把一个完整的 MP3 帧切断，解码层需要一个 **拼装缓冲区 (3~4KB)**，将不够一帧的数据留下，与下次网络包拼接后再解码。
3. **PCM 消化层 (iis.cpp)**：一旦 `mp3dec_decode_frame` 成功解出一帧（通常为 1152×2 个 short 采样），立刻调用 `iis::data_write()` 将数据塞进 I2S 的 DMA 队列。你的 `reduce_buffer_if_needed` 和 `fill_buffer_if_needed` 会自动处理网络的延迟与积压。

---

## 2. DLNA 播放核心代码实现

不需要引入 `ex` 扩展中的繁杂 IO 回调，仅仅依靠核心 API 即可实现。你可以专门创建一个 `audio_play.cpp` 或 `dlna_player.cpp` 并引入下述逻辑：

```cpp
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "minimp3.h"
#include "iis.hpp" // 你的底层音频缓冲

// 定义一个缓冲区，用于暂存被网络包截断的“半个”MP3帧，建议4KB（足够容纳最大可能的单帧数据）
#define MP3_IN_BUF_SIZE 4096 
static uint8_t g_mp3_in_buf[MP3_IN_BUF_SIZE];
static int g_mp3_in_len = 0;

static mp3dec_t g_mp3d;
// PCM 解码结果缓冲区，单帧最大采样数为 1152双声道 (1152 * 2 = 2304个short)
static mp3d_sample_t g_pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];

/**
 * @brief DLNA 播放器初始化
 */
void dlna_player_init() {
    // 初始化解码器状态
    mp3dec_init(&g_mp3d);
    g_mp3_in_len = 0;
    
    // 初始化 IIS 和 DMA (只调一次)
    // 假设你的 iis() 构造函数或者单例已经在其它地方启动了
}

/**
 * @brief 当从网络 socket 接收到一部分 MP3 数据包时调用此函数
 * @param net_data 接收到的裸数据指针
 * @param net_len 接收到的字节长度
 */
void dlna_on_network_data_received(const uint8_t *net_data, int net_len) {
    if (net_data == nullptr || net_len <= 0) return;

    // 1. 将新收到的数据追加到我们的缓存区末尾
    // 注意：实际项目中需判断 (g_mp3_in_len + net_len) <= MP3_IN_BUF_SIZE 以防止溢出。
    // 如果溢出说明网络推流进来的都是无效垃圾数据，无法解出有效帧，需主动清空。
    if (g_mp3_in_len + net_len > MP3_IN_BUF_SIZE) {
        osal_printk("MP3 Buffer Overflow! Resyncing...\n");
        g_mp3_in_len = 0; // 丢弃无效数据，重新同步
    }
    memcpy(g_mp3_in_buf + g_mp3_in_len, net_data, net_len);
    g_mp3_in_len += net_len;

    int offset = 0;

    // 2. 循环尝试解码，只要里面还有完整的一帧就一直解
    while (offset < g_mp3_in_len) {
        mp3dec_frame_info_t info;
        
        // 核心：解码从 offset 开始的剩余数据
        int samples = mp3dec_decode_frame(&g_mp3d, g_mp3_in_buf + offset, g_mp3_in_len - offset, g_pcm_buf, &info);

        if (info.frame_bytes == 0) {
            // 返回值为0代表剩余数据不够组成一帧（或者没找到同步头）
            // 需要等待网络继续发包，跳出本轮解码
            break;
        }

        // --- 成功解出一帧！ ---

        // 如果音频流在此变了采样率，你可以在这里让 iis 硬件适配
        // (在你的 iis.cpp 中：uapi_i2s_set_sample_rate)
        // if (info.hz != current_hw_hz) { adjust_iis_bclk(info.hz); }

        if (samples > 0) {
            // 用你写的机制写入 DMA：
            // samples 是单声道的采样数(例如 1152)，需要乘上声道数才是实际的 int16 个数。
            // 注意: minimp3 可能会解出单声道音频(info.channels == 1)，
            // 如果你的硬件 I2S 强行配置为了2声道，在此处要做“双声道复制补齐”。
            uint32_t total_pcm_shorts = samples * info.channels;
            
            if (info.channels == 1) {
                // 【针对单声道做的适配】：把1声道复制为左右一样
                static mp3d_sample_t stereo_buf[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
                for(int i=0; i<samples; i++) {
                    stereo_buf[i*2] = g_pcm_buf[i];
                    stereo_buf[i*2+1] = g_pcm_buf[i];
                }
                iis::data_write(stereo_buf, samples * 2);
            } else {
                // 正常的立体声，直接喂给你的 iis::data_write !
                iis::data_write(g_pcm_buf, total_pcm_shorts);
            }
        }

        // 把 offset 往前推（滑过已经被吃掉的帧数据）
        offset += info.frame_bytes;
    }

    // 3. 把“剩下的、不够一帧”的尾巴数据，挪到缓冲区的最前面，等下次网络包拼接
    int remain = g_mp3_in_len - offset;
    if (remain > 0) {
        memmove(g_mp3_in_buf, g_mp3_in_buf + offset, remain);
    }
    g_mp3_in_len = remain;
}

/**
 * @brief DLNA 停止或切歌时调用
 */
void dlna_player_stop() {
    iis::data_clear();   // 清空你 iis 的 DMA 缓冲
    g_mp3_in_len = 0;    // 清空解码前拼装缓冲
}
```

---

## 3. 为什么这种模式完美契合你的 `iis.cpp`？

结合你的 `iis.cpp` 可以看出，你的底层其实相当健壮：
*   **自带抗抖动（Jitter Buffer）**：你的 `iis::data_write` 里有 `pending_frames` 计数和 `prebuffer_num`。这意味着当网络推流稍慢时，`has_ready` 被阻塞直到缓存蓄满一定限度才真的让 DMA 起跑。这极为出色，恰好符合网络音频流防卡顿的设计核心。
*   **自带重放防破音（Fill Buffer）**：当网络卡顿时，`fill_buffer_if_needed` 会复制最后一个采样点（产生一定程度的“长音”来代替静音爆音）。
*   **零内存拷贝开销的 DMA 对齐**：`minimp3` 解在本地临时数组（`g_pcm_buf`），交到你的 `data_write` 后直接触发 cache clean 交给 LLI（链表 DMA）去刷硬件 FIFO。

综上，采用**网络接收直接调用函数推流 -> 寻找帧并解码 -> 调用 `iis::data_write` -> DMA 播放**的链路，没有跨越过多的层级，是最轻量且最优的。完全不需要 `minimp3_ex.h` 所提供的高级拖拽和虚拟文件接口。