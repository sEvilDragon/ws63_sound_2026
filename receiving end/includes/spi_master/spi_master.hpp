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
 * 参照官方 spi_master_demo.c:
 *  - 固定传输长度, tx_bytes == rx_bytes
 *  - sste = 0, wait_cycles = 0x10
 *  - 先 master_write 再 master_read
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
