#pragma once

extern "C" {
#include "spi.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "app_init.h"
}

namespace sed_ws63 {

/**
 * @brief SPI Master —— 接收端 SPI_BUS_1 作为主机与控制端通讯
 *
 * DMA 模式 (CONFIG_SPI_SUPPORT_DMA=y):
 *  - uapi_spi_master_writeread() → spi_writeread_dma()
 *  - TX/RX 在同一组 SCK 周期并行完成, 只交换 TRANSFER_LEN 字节
 *  - 不会多送零值到从机, 从机 RX FIFO 不会累积溢出
 *  - 非 DMA 回退: hal_spi_write + hal_spi_read 轮询路径
 */
class spi_master {
public:
    spi_master();
    ~spi_master() = default;

    /**
     * @brief 发送数据并接收从机回复(阻塞式)
     * @param tx_data 发送数据
     * @param tx_len  发送字节数(会填充到 TRANSFER_LEN)
     * @param rx_data 接收缓冲区
     * @param rx_len  期望接收字节数(应为 TRANSFER_LEN)
     * @return 0 成功, 非0 失败
     */
    int transfer(const uint8_t *tx_data, uint32_t tx_len, uint8_t *rx_data, uint32_t rx_len);

    static constexpr uint32_t TRANSFER_LEN = 16; // 固定传输长度

private:
    void pin_init();
    void spi_init();

    static constexpr spi_bus_t BUS = SPI_BUS_1;
    static constexpr uint32_t FREQ_MHZ = 2;
    static constexpr uint32_t TIMEOUT = 0xFFFFFFFF; // 无限等待

    uint8_t tx_buffer[TRANSFER_LEN]; // 预分配发送缓冲
    uint8_t rx_buffer[TRANSFER_LEN]; // 预分配接收缓冲
};

} // namespace sed_ws63
