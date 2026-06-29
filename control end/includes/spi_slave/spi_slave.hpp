#pragma once

extern "C" {
#include "spi.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "app_init.h"
}

namespace sed_ws63 {

/**
 * @brief SPI Slave —— 控制端 SPI_BUS_0 作为从机与接收端通讯
 *
 * DMA 模式 (CONFIG_SPI_SUPPORT_DMA=y):
 *  - uapi_spi_slave_writeread() → spi_writeread_dma()
 *  - TX/RX 在同一组 SCK 周期并行完成, 只交换 TRANSFER_LEN 字节
 *  - 不会有额外的零值发送, 从机 RX FIFO 不会累积溢出
 *  - 非 DMA 回退: hal_spi_write + hal_spi_read 轮询路径
 */
class spi_slave {
public:
    spi_slave();
    ~spi_slave() = default;

    /**
     * @brief 等待主机数据并回复(阻塞式)
     * @param rx_data 接收缓冲区
     * @param rx_len  期望接收字节数(应为 TRANSFER_LEN)
     * @param tx_data 回复数据
     * @param tx_len  回复字节数(会填充到 TRANSFER_LEN)
     * @return 0 成功, 非0 失败
     */
    int transfer(uint8_t *rx_data, uint32_t rx_len, const uint8_t *tx_data, uint32_t tx_len);

    static constexpr uint32_t TRANSFER_LEN = 16; // 固定传输长度

private:
    void pin_init();
    void spi_init();

    static constexpr spi_bus_t BUS = SPI_BUS_0;
    static constexpr uint32_t TIMEOUT = 0xFFFFFFFF;

    uint8_t tx_buffer[TRANSFER_LEN];
    uint8_t rx_buffer[TRANSFER_LEN];
};

} // namespace sed_ws63
