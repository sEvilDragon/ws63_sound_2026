#pragma once

extern "C" {
#include "pinctrl.h"
#include "gpio.h"
#include "soc_osal.h"
}

namespace sed_ws63 {

/**
 * @brief DWS (Dual-Wire Serial) Slave —— 控制端通过 2 线 GPIO 与接收端通讯
 *
 * 物理层:
 *  - CLK (GPIO_07): Master 驱动, Slave 输入 (轮询边沿)
 *  - DAT (GPIO_11): 双向, 模拟开漏 (OUTPUT=LOW / INPUT=释放, 需外部上拉)
 *
 * 帧同步:
 *  - 从机检测 START (CLK 空闲 HIGH→LOW) 后开始接收
 *  - 数 128 个 CLK 上升沿后自动切换到发送模式
 *  - 再发 48 bits 后释放 DAT, 回到等待 START 状态
 *
 * 接口兼容原 spi_slave: transfer(rx, rx_len, tx, tx_len)
 */
class dws_slave {
public:
    dws_slave();
    ~dws_slave() = default;

    /**
     * @brief 等待主机时钟并完成双向传输(阻塞式)
     * @param rx_data 接收缓冲区
     * @param rx_len  期望接收字节数 (≤ TRANSFER_LEN)
     * @param tx_data 回复数据 (前 SPI_SETTINGS_LEN 字节有效)
     * @param tx_len  回复字节数 (≤ TRANSFER_LEN, 不足补零)
     * @return 0 成功, 非0 失败
     */
    int transfer(uint8_t *rx_data, uint32_t rx_len, const uint8_t *tx_data, uint32_t tx_len);

    static constexpr uint32_t TRANSFER_LEN = 16;

private:
    void pin_init();

    static constexpr pin_t CLK_PIN = GPIO_07;
    static constexpr pin_t DAT_PIN = GPIO_11;

    uint8_t recv_bit();
    void send_bit(uint8_t bit_val);

    uint8_t tx_buffer[TRANSFER_LEN];
    uint8_t rx_buffer[TRANSFER_LEN];
};

} // namespace sed_ws63
