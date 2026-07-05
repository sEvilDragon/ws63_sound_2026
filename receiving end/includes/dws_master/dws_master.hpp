#pragma once

extern "C" {
#include "pinctrl.h"
#include "gpio.h"
#include "soc_osal.h"
}

namespace sed_ws63 {

/**
 * @brief DWS (Dual-Wire Serial) Master —— 接收端通过 2 线 GPIO 与控制端通讯
 *
 * 物理层:
 *  - CLK (GPIO_06): Master 推挽输出, Slave 输入
 *  - DAT (GPIO_01): 双向, 模拟开漏 (OUTPUT=LOW / INPUT=释放, 需外部上拉)
 *
 * 帧结构:
 *  [START 100μs] [16B M→S] [TURN 200μs] [6B S→M] [IDLE]
 *
 * 位时序 (保守):
 *  - 半周期 20μs → 25 kbps
 *  - 一帧 ≈ 7.5ms, 远小于 50ms 任务周期
 *
 * 接口兼容原 spi_master: transfer(tx, tx_len, rx, rx_len)
 */
class dws_master {
public:
    dws_master();
    ~dws_master() = default;

    /**
     * @brief 发送数据并接收从机回复(阻塞式)
     * @param tx_data 发送数据 (前 SPI_SETTINGS_LEN 字节为设置, 后跟音频数据)
     * @param tx_len  发送字节数 (≤ TRANSFER_LEN, 不足补零)
     * @param rx_data 接收缓冲区
     * @param rx_len  期望接收字节数 (≤ TRANSFER_LEN)
     * @return 0 成功, 非0 失败
     */
    int transfer(const uint8_t *tx_data, uint32_t tx_len, uint8_t *rx_data, uint32_t rx_len);

    static constexpr uint32_t TRANSFER_LEN = 16;
    static constexpr uint32_t REPLY_LEN = 8;

private:
    void pin_init();

    static constexpr pin_t CLK_PIN = GPIO_06;
    static constexpr pin_t DAT_PIN = GPIO_01;

    /* 位时序: 半周期 20μs, 一个完整 bit = 40μs → 25 kbps */
    static constexpr uint32_t HALF_BIT_US = 20;
    /* START 条件: CLK 低 100μs */
    static constexpr uint32_t START_US = 100;
    /* 转向间隔: DAT 释放后等 200μs 让 Slave 准备发送 */
    static constexpr uint32_t TURNAROUND_US = 200;

    void send_bit(uint8_t bit_val);
    uint8_t recv_bit();

    uint8_t tx_buffer[TRANSFER_LEN];
    uint8_t rx_buffer[TRANSFER_LEN];
};

} // namespace sed_ws63
