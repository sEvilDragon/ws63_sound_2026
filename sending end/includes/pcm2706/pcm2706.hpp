#ifndef __PCM2706_HPP__
#define __PCM2706_HPP__

#include <array>
extern "C" {
// 此处宏定义用于去除vsc智能报错的错误，去除并不影响实际编译结果
#define CONFIG_DMA_SUPPORT_LLI 1
#include "gpio.h"
#include "i2s.h"
#include "soc_osal.h"
#include "pinctrl.h"
#include "dma.h"
#include "hal_sio.h"
#include "hal_dma.h"
#include "sio_porting.h"
#include "sio_porting.h"
#include "osal_cache.h"
#include "osal_addr.h"
#include "osal_semaphore.h"
#include "osal_mutex.h"
}

class pcm2706 {
public:
    pcm2706();

private:
    static void pin_init();
    static void i2s_dma_init_1();
    static void i2s_init();
    static void i2s_dma_init_2();
    static void sem_mutex_init();
    static void dma_lli_init();
    static void i2s_read_callback(uint8_t intr, uint8_t channel, uintptr_t arg);

public:
    static std::array<void *, 100> raw_buffers;    // 用于 kfree
    static std::array<int16_t *, 100> dma_buffers; // 用于实际读写
    static constexpr uint32_t buffer_size = 960;   // 传输数据大小
    static constexpr uint32_t buffer_num = 100;    // 传输缓冲区数量
    static constexpr uint16_t cache_size = 32;     // Cache Line大小

    static osal_semaphore is_data_ready;   // 数据准备就绪信号量
    static osal_semaphore is_buffer_empty; // 缓冲区空信号量
    static osal_mutex dma_mutex;           // DMA操作互斥锁

    // 定义互斥锁相关的一些变量
    static volatile int write_idx;      // 当前写入缓冲区索引
    static volatile int read_idx;       // 当前读取缓冲区索引
    static volatile int pending_frames; // 待处理的音频帧数量

private:
    // 定义引脚
    static constexpr pin_t mcsl_pin = GPIO_07;
    static constexpr pin_t sclk_pin = GPIO_10;
    static constexpr pin_t lrclk_pin = GPIO_11;
    static constexpr pin_t datain_pin = GPIO_12;

    static constexpr sio_bus_t i2s_num = SIO_BUS_0; // ws63只有一个iis

    // 定义iis 相关的一些变量
    static constexpr uint8_t drive_mode = 0;
    static constexpr uint8_t transfer_mode = 0;
    static constexpr uint8_t data_width = 1;       // 默认16位数据宽度
    static constexpr uint8_t channel_num_sio = 0;  // 寄存器双声道
    static constexpr uint8_t timing = 0;           // 默认时序
    static constexpr uint8_t clk_edge = 1;         // 上升沿采样
    static constexpr uint8_t div_num = 16;         // 16位数据
    static constexpr uint8_t channel_num_true = 2; // 双声道

    static constexpr i2s_sample_rate_t sample_rate = I2S_SAMPLE_RATE_48K; // 采样率

    // 定义dma_iis 相关的一些变量
    static constexpr bool tx_dma_enable = false;
    static constexpr uint8_t tx_threshold = 8; // 默认传输阈值
    static constexpr bool rx_dma_enable = true;
    static constexpr uint8_t rx_threshold = 4;

    // 定义dma lli 相关的一些变量
    static uint8_t dma_channel; // DMA通道号，实际值由系统分配

    // 源地址（I2S数据寄存器地址，需根据实际情况设置）
    static constexpr uint16_t transfer_size = (uint16_t)buffer_size;        // 传输数据大小
    static constexpr uint16_t src_handshaking = HAL_DMA_HANDSHAKING_I2S_RX; // I2S RX握手信号
    static constexpr uint16_t dest_handshaking = 0;                         // 不使用目的端握手
    static constexpr uint8_t transfer_type =
        HAL_DMA_TRANS_PERIPHERAL_TO_MEMORY_DMA; // 传输类型：外设到内存且使用DMA控制传输
    static constexpr uint8_t trans_dir = HAL_DMA_TRANSFER_DIR_PERIPHERAL_TO_MEM; // 传输方向：外设到内存
    static constexpr uint8_t priority = 1;                                       // 传输优先度
    static constexpr uint8_t src_width = 1;                                      // 源地址数据宽度，默认16位
    static constexpr uint8_t dest_width = 1;                                     // 目的地址数据宽度，默认16位
    static constexpr uint8_t burst_length = 1;                                   // 突发传输长度，0表示单次传输
    static constexpr uint8_t src_increment = 2;                                  // 源地址递增，外设寄存器地址固定
    static constexpr uint8_t dest_increment = 0;                                 // 目的地址递增，内存地址递增
    static constexpr uint8_t protection = 0;                                     // 传输保护属性，默认无特殊保护
};

#endif