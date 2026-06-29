#include "spi_master.hpp"

namespace sed_ws63 {

spi_master::spi_master()
{
    osal_printk("[SPI_Master] ctor: pin_init...\r\n");
    pin_init();
    osal_printk("[SPI_Master] ctor: spi_init...\r\n");
    spi_init();
    osal_printk("[SPI_Master] SPI_BUS_1 DMA 初始化完成\r\n");
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
        return;
    }

    /* DMA 模式: 一次 writeread 完成双向传输, TX/RX 在同一组 SCK 周期
     * 注意: uapi_dma_init/open 已在 app_entry 中统一调用, 此处只需 set_dma_mode */
    spi_dma_config_t dma_cfg = {.src_width = 0, .dest_width = 0, .burst_length = 0, .priority = 0};
    ret = uapi_spi_set_dma_mode(BUS, true, &dma_cfg);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SPI_Master] DMA 模式设置失败, ret=0x%x, 回退到轮询模式\r\n", ret);
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

    /*
     * DMA 模式: uapi_spi_master_writeread() 内部走 spi_writeread_dma(),
     * TX/RX 在同一组 SCK 周期完成, 只发 16B 收 16B, 不会多送零值,
     * 从机 RX FIFO 不会累积溢出。
     * 非 DMA 回退: 走 hal_spi_write + hal_spi_read 轮询路径。
     */
    errcode_t ret = uapi_spi_master_writeread(BUS, &data, TIMEOUT);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SPI_Master] writeread 失败, ret=0x%x\r\n", ret);
        return -1;
    }

    uint32_t copy_len = (rx_len < TRANSFER_LEN) ? rx_len : TRANSFER_LEN;
    for (uint32_t i = 0; i < copy_len; i++) {
        rx_data[i] = rx_buffer[i];
    }
    return 0;
}

} // namespace sed_ws63
