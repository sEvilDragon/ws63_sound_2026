#include "spi_master.hpp"

namespace sed_ws63 {

spi_master::spi_master()
{
    pin_init();
    spi_init();
    osal_printk("[SPI_Master] SPI_BUS_1 初始化完成\r\n");
}

void spi_master::pin_init()
{
    uapi_pin_set_mode(GPIO_00, PIN_MODE_3); // CSN
    uapi_pin_set_mode(GPIO_01, PIN_MODE_3); // MOSI
    uapi_pin_set_mode(GPIO_03, PIN_MODE_3); // MISO
    uapi_pin_set_mode(GPIO_06, PIN_MODE_3); // SCK
}

void spi_master::spi_init()
{
    spi_attr_t config = {0};
    spi_extra_attr_t ext_config = {0};

    config.is_slave = false;
    config.slave_num = 1; // SPI_SLAVE0
    config.bus_clk = SPI_CLK_FREQ;
    config.freq_mhz = FREQ_MHZ;
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
        osal_printk("[SPI_Master] 初始化失败, ret=0x%x\r\n", ret);
    }
}

int spi_master::transfer(const uint8_t *tx_data, uint32_t tx_len, uint8_t *rx_data, uint32_t rx_len)
{
    if (tx_len > TRANSFER_LEN || rx_len > TRANSFER_LEN) {
        return -1;
    }

    for (uint32_t i = 0; i < TRANSFER_LEN; i++) {
        tx_buffer[i] = (i < tx_len) ? tx_data[i] : 0;
        rx_buffer[i] = 0;
    }

    spi_xfer_data_t data = {
        .tx_buff = tx_buffer,
        .tx_bytes = TRANSFER_LEN,
        .rx_buff = rx_buffer,
        .rx_bytes = TRANSFER_LEN,
    };

    // 参照官方 demo: 先 master_write 再 master_read
    errcode_t ret = uapi_spi_master_write(BUS, &data, TIMEOUT);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SPI_Master] 发送失败, ret=0x%x\r\n", ret);
        return -1;
    }

    ret = uapi_spi_master_read(BUS, &data, TIMEOUT);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SPI_Master] 接收失败, ret=0x%x\r\n", ret);
        return -1;
    }

    uint32_t copy_len = (rx_len < TRANSFER_LEN) ? rx_len : TRANSFER_LEN;
    for (uint32_t i = 0; i < copy_len; i++) {
        rx_data[i] = rx_buffer[i];
    }
    return 0;
}

} // namespace sed_ws63
