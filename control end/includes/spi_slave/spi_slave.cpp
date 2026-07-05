#include "spi_slave.hpp"

extern "C" {
#include "dma_porting.h"
#include "hal_dma.h"
}

namespace sed_ws63 {

spi_slave::spi_slave()
{
    pin_init();
    spi_init();
}

void spi_slave::pin_init()
{
    uapi_pin_set_mode(GPIO_07, PIN_MODE_3); // CLK
    uapi_pin_set_mode(GPIO_10, PIN_MODE_3); // CS
    uapi_pin_set_mode(GPIO_11, PIN_MODE_3); // DI (MOSI)
    uapi_pin_set_mode(GPIO_09, PIN_MODE_3); // DO (MISO)
}

void spi_slave::spi_init()
{
    spi_attr_t config = {0};
    spi_extra_attr_t ext_config = {0};

    config.is_slave = true;
    config.slave_num = 1;
    config.bus_clk = SPI_CLK_FREQ;
    config.freq_mhz = 2;
    config.clk_polarity = SPI_CFG_CLK_CPOL_0;
    config.clk_phase = SPI_CFG_CLK_CPHA_0;
    config.frame_format = SPI_CFG_FRAME_FORMAT_MOTOROLA_SPI;
    config.spi_frame_format = HAL_SPI_FRAME_FORMAT_STANDARD;
    config.frame_size = HAL_SPI_FRAME_SIZE_8;
    config.tmod = HAL_SPI_TRANS_MODE_TXRX;
    config.sste = 0;
    config.ndf = 0;

    ext_config.sspi_param.wait_cycles = 0x10;

    errcode_t ret = uapi_spi_init(BUS, &config, &ext_config);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLAVE] *** FATAL: uapi_spi_init 失败! ret=0x%X ***\r\n", (unsigned)ret);
        return;
    }

    /* DMA 模式: 一次 writeread 完成双向传输, TX/RX 在同一组 SCK 周期, 不会产生多余零值
     * 注意: uapi_dma_init/open 已在 app_entry 中统一调用, 此处只需 set_dma_mode */
    spi_dma_config_t dma_cfg = {.src_width = 0, // 8-bit, 匹配 HAL_SPI_FRAME_SIZE_8
                                .dest_width = 0,
                                .burst_length = 0,
                                .priority = 0};
    ret = uapi_spi_set_dma_mode(BUS, true, &dma_cfg);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLAVE] *** DMA 模式设置失败! ret=0x%X, 将走轮询路径 ***\r\n", (unsigned)ret);
    } else {
                    (unsigned)dma_cfg.dest_width, (unsigned)dma_cfg.burst_length, (unsigned)dma_cfg.priority);
    }
}

int spi_slave::transfer(uint8_t *rx_data, uint32_t rx_len, const uint8_t *tx_data, uint32_t tx_len)
{
    if (rx_len > TRANSFER_LEN || tx_len > TRANSFER_LEN) {
        return -1;
    }

    for (uint32_t i = 0; i < TRANSFER_LEN; i++) {
        rx_buffer[i] = 0;
        tx_buffer[i] = (i < tx_len) ? tx_data[i] : 0;
    }

    spi_xfer_data_t data = {
        .tx_buff = tx_buffer,
        .tx_bytes = TRANSFER_LEN,
        .rx_buff = rx_buffer,
        .rx_bytes = TRANSFER_LEN,
    };

    /*
     * WS63 的 HAL_SPI_DEVICE_MODE_SET_REG (0x44000250) 只有 1 个 bit 全局控制
     * 所有 SPI 的主从模式。BUS_0 需 Slave, BUS_1 需 Master, 无法静态共存。
     * 解决方案: 每次传输前动态切换到正确的模式。
     * 传输极短(16B@2MHz=64μs), 碰撞概率极低, 即使碰撞也只丢一帧, 上层会重试。
     */
    spi_porting_set_device_mode(BUS, SPI_MODE_SLAVE);
    errcode_t ret = uapi_spi_slave_writeread(BUS, &data, TIMEOUT);
    if (ret != ERRCODE_SUCC) {
        // 详细打印错误码, 帮助定位是 DMA 配置失败 / sem 超时 / FIFO 溢出
        osal_printk("[SLAVE] writeread FAIL: ret=0x%X (dec=%d) tx_bytes=%u rx_bytes=%u\r\n", (unsigned)ret, (int)ret,
                    (unsigned)data.tx_bytes, (unsigned)data.rx_bytes);
        return -1;
    }

    uint32_t copy_len = (rx_len < TRANSFER_LEN) ? rx_len : TRANSFER_LEN;
    for (uint32_t i = 0; i < copy_len; i++) {
        rx_data[i] = rx_buffer[i];
    }

    return 0;
}

} // namespace sed_ws63
