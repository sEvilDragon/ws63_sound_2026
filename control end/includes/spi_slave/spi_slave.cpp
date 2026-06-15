#include "spi_slave.hpp"

namespace sed_ws63 {

spi_slave::spi_slave()
{
    pin_init();
    spi_init();
    osal_printk("[SPI_Slave] SPI_BUS_0 初始化完成\r\n");
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
        osal_printk("[SPI_Slave] 初始化失败, ret=0x%x\r\n", ret);
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

    // 参照官方 demo: 先 slave_read 再 slave_write
    errcode_t ret = uapi_spi_slave_read(BUS, &data, TIMEOUT);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SPI_Slave] 接收失败, ret=0x%x\r\n", ret);
        return -1;
    }

    uint32_t copy_len = (rx_len < TRANSFER_LEN) ? rx_len : TRANSFER_LEN;
    for (uint32_t i = 0; i < copy_len; i++) {
        rx_data[i] = rx_buffer[i];
    }

    ret = uapi_spi_slave_write(BUS, &data, TIMEOUT);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SPI_Slave] 发送失败, ret=0x%x\r\n", ret);
        return -1;
    }
    return 0;
}

} // namespace sed_ws63
