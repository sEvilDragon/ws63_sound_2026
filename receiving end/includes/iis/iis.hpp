#ifndef __IIS_HPP__
#define __IIS_HPP__

extern "C" {
#include "gpio.h"
#include "i2s.h"
#include "dma.h"
#include "hal_dma.h"
#include "sio_porting.h"
#include "osal_cache.h"
#include "pinctrl.h"
#include "hal_sio.h"
#include "soc_osal.h"
#include "hal_sio.h"
#include "osal_addr.h"
}
#include <array>

class iis {
public:
    iis();
    ~iis();
    static void data_write(const int16_t *data, uint32_t size, uint8_t volume, uint8_t bass);
    // 定义清理函数
    static void data_clear();
    static void data_clear_one(int index);
    // 补帧算法
    static void fill_buffer_if_needed();
    static void reduce_buffer_if_needed(uint32_t &size);
    // 调整iis的频率
    static void set_rate_of_iis(i2s_sample_rate_t rate);

private:
    // 初始化引脚
    static void pin_init();
    static void i2s_dma_init_1();
    static void i2s_init();
    static void i2s_dma_init_2();
    static void sem_mutex_init();
    static void dma_lli_init();
    static void i2s_send_callback(uint8_t intr, uint8_t channel, uintptr_t arg);

public:
    static std::array<void *, 100> raw_buffers;    // 用于 kfree
    static std::array<int16_t *, 100> dma_buffers; // 用于实际读写
    static constexpr uint32_t buffer_size = 960;   // 传输数据大小
    static constexpr uint32_t buffer_num = 45;     // 传输缓冲区数量
    static constexpr uint16_t cache_size = 32;     // Cache Line大小

    static const uint16_t volume_gain_table[101];

    // 定义缓冲区
    static constexpr int prebuffer_num = buffer_num * 0.6;      // 低水位恢复门限，避免长时间停播等待
    static constexpr int min_buffer_num = buffer_num * 0.05;    // 仅在几乎耗尽时才停播，降低启停抖动
    static constexpr uint8_t if_small_num = buffer_num * 0.25;  // 补帧/删除帧数量
    static constexpr uint8_t if_fill_num = 5;                   // 补帧预设值
    static constexpr uint8_t if_reduce_num = buffer_num * 0.85; // 删除帧预设值

    // 定义互斥锁相关的一些变量
    static volatile int write_idx;         // 当前写入缓冲区索引
    static volatile int read_idx;          // 当前读取缓冲区索引
    static volatile int pending_frames;    // 待处理的音频帧数
    static volatile uint32_t write_offset; // 当前写槽内偏移，flush后必须同步复位

    static bool is_ready; // 标志位，表示PCM5102是否准备发送数据
private:
    // 定义引脚
    static constexpr pin_t mcsl_pin = GPIO_07;
    static constexpr pin_t sclk_pin = GPIO_10;
    static constexpr pin_t lrclk_pin = GPIO_11;
    static constexpr pin_t dataout_pin = GPIO_09;

    static constexpr sio_bus_t i2s_num = SIO_BUS_0; // ws63只有一个iis

    // 定义I2S配置参数
    static constexpr uint8_t drive_mode = 1;       // master模式
    static constexpr uint8_t transfer_mode = 0;    // DMA模式
    static constexpr uint8_t data_width = 1;       // 16位数据宽度
    static constexpr uint8_t channel_num = 0;      // 双声道
    static constexpr uint8_t timing = 0;           // 默认
    static constexpr uint8_t clk_edge = 1;         // 时钟上升沿采样
    static constexpr uint8_t div_num = 16;         // 分频系数，具体值需要根据采样率和主频计算
    static constexpr uint8_t channel_num_true = 2; // 实际声道数，双声道

    // 定义频率
    static constexpr i2s_sample_rate_t i2s_sample_rate =
        I2S_SAMPLE_RATE_44K; // 采样率，8对应48kHz，具体值需要根据I2S驱动文档确认

    // 定义iis DMA配置参数
    static constexpr bool tx_dma_enable = true;
    static constexpr uint8_t tx_threshold = 7;
    static constexpr bool rx_dma_enable = false;
    static constexpr uint8_t rx_threshold = 1;

    // 定义dma lli 相关的一些变量
    static uint8_t dma_channel; // DMA通道号，实际值由系统分配

    // 记录最后的左右帧
    static int16_t last_left_sample;
    static int16_t last_right_sample;

    static float biquad_bx1_l, biquad_bx2_l, biquad_by1_l, biquad_by2_l;
    static float biquad_bx1_r, biquad_bx2_r, biquad_by1_r, biquad_by2_r;

    // 源地址（I2S数据寄存器地址，需根据实际情况设置）
    static constexpr uint16_t transfer_size = (uint16_t)buffer_size;         // 传输数据大小
    static constexpr uint16_t src_handshaking = 0;                           // 不使用源端握手
    static constexpr uint16_t dest_handshaking = HAL_DMA_HANDSHAKING_I2S_TX; // I2S TX握手信号
    static constexpr uint8_t transfer_type =
        HAL_DMA_TRANS_MEMORY_TO_PERIPHERAL_DMA; // 传输类型：内存到外设且使用DMA控制传输
    static constexpr uint8_t trans_dir = HAL_DMA_TRANSFER_DIR_MEM_TO_PERIPHERAL; // 传输方向：内存到外设
    static constexpr uint8_t priority = 1;                                       // 传输优先度
    static constexpr uint8_t src_width = 1;                                      // 源地址数据宽度，默认16位
    static constexpr uint8_t dest_width = 1;                                     // 目的地址数据宽度，默认16位
    static constexpr uint8_t burst_length = 0;                                   // 突发传输长度，0表示单次传输
    static constexpr uint8_t src_increment = 0;                                  // 源地址递增，内存地址递增
    static constexpr uint8_t dest_increment = 2;                                 // 目的地址固定
    static constexpr uint8_t protection = 2;                                     // 保护属性
};

#endif