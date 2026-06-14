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
 * 通讯参数(双方统一):
 *  - 帧格式: Motorola SPI, Standard 单线, 8-bit
 *  - CPOL=0, CPHA=0 (Mode 0)
 *  - 时钟: 2MHz (bus_clk=32MHz 分频)
 *  - SSTE 使能(帧间 CS 翻转)
 *  - 全双工 TXRX
 */
class spi_master {
public:
    spi_master();
    ~spi_master() = default;

    /**
     * @brief 向从机发送一帧数据(含帧头 + 长度 + 载荷)
     * @param data  载荷数据指针
     * @param len   载荷字节数 (0~255)
     * @return 0 成功, 非0 失败
     */
    int send(const uint8_t *data, uint8_t len);

    /**
     * @brief 从从机读取一帧数据(含帧头 + 长度 + 载荷)
     * @param data  接收缓冲区
     * @param max_len 缓冲区最大容量
     * @return 实际接收到的载荷字节数, 负数表示失败
     */
    int recv(uint8_t *data, uint8_t max_len);

    /**
     * @brief 全双工收发: 发送一帧同时接收一帧
     * @param tx_data 发送载荷
     * @param tx_len  发送载荷字节数
     * @param rx_data 接收缓冲区
     * @param rx_max  接收缓冲区最大容量
     * @return 实际接收到的载荷字节数, 负数表示失败
     */
    int writeread(const uint8_t *tx_data, uint8_t tx_len, uint8_t *rx_data, uint8_t rx_max);

private:
    void pin_init();
    void spi_init();

    static constexpr spi_bus_t BUS = SPI_BUS_1;
    static constexpr uint32_t FREQ_MHZ = 2;
    static constexpr uint32_t TIMEOUT = 10000;

    // 帧协议常量
    static constexpr uint8_t FRAME_HEADER = 0xA5;
    static constexpr uint8_t FRAME_HDR_SIZE = 2;                             // 帧头(1B) + 长度(1B)
    static constexpr uint8_t MAX_PAYLOAD = 255;                              // 最大载荷
    static constexpr uint16_t MAX_FRAME_SIZE = FRAME_HDR_SIZE + MAX_PAYLOAD; // 257
};

} // namespace sed_ws63
