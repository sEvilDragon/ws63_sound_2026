#include "sk9822_led.hpp"

namespace sed_ws63 {

uint8_t sk9822_led::frame_buf[sk9822_led::FRAME_SIZE];

sk9822_led::sk9822_led()
{
    pin_init();
    spi_init();
    spi_dma_init();
    clear();
    osal_printk("[SK9822] SPI_BUS_1 LED 初始化完成, %d 颗灯珠\r\n", NUM_LEDS);
}

void sk9822_led::pin_init()
{
    uapi_pin_set_mode(GPIO_01, PIN_MODE_3); // MOSI
    uapi_pin_set_mode(GPIO_06, PIN_MODE_3); // SCK
}

void sk9822_led::spi_init()
{
    spi_attr_t config = {0};
    spi_extra_attr_t ext_config = {0};

    config.is_slave = false;
    config.slave_num = 1;
    config.bus_clk = SPI_CLK_FREQ;
    config.freq_mhz = FREQ_MHZ;
    config.clk_polarity = SPI_CFG_CLK_CPOL_0;
    config.clk_phase = SPI_CFG_CLK_CPHA_0;
    config.frame_format = SPI_CFG_FRAME_FORMAT_MOTOROLA_SPI;
    config.spi_frame_format = HAL_SPI_FRAME_FORMAT_STANDARD;
    config.frame_size = HAL_SPI_FRAME_SIZE_8;
    config.tmod = HAL_SPI_TRANS_MODE_TX;
    config.sste = 0;
    config.ndf = 0;

    ext_config.sspi_param.wait_cycles = 0x10;

    errcode_t ret = uapi_spi_init(BUS, &config, &ext_config);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SK9822] SPI 初始化失败, ret=0x%x\r\n", ret);
    }
}

void sk9822_led::spi_dma_init()
{
    /* 注意: uapi_dma_init/open 已在 app_entry 中统一调用, 此处只需 set_dma_mode */
    spi_dma_config_t dma_cfg = {0};
    dma_cfg.src_width = 0;    // 1 byte
    dma_cfg.dest_width = 0;   // 1 byte
    dma_cfg.burst_length = 0; // burst 1
    dma_cfg.priority = 0;

    errcode_t ret = uapi_spi_set_dma_mode(BUS, true, &dma_cfg);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SK9822] DMA 模式设置失败, ret=0x%x\r\n", ret);
    }
}

void sk9822_led::spi_send(const uint8_t *data, uint32_t len)
{
    spi_xfer_data_t xfer = {0};
    xfer.tx_buff = (uint8_t *)data;
    xfer.tx_bytes = len;

    errcode_t ret = uapi_spi_master_write(BUS, &xfer, TIMEOUT);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SK9822] SPI write 失败, ret=0x%x\r\n", ret);
    }
}

void sk9822_led::set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    if (index >= NUM_LEDS) return;
    if (brightness > 31) brightness = 31;

    uint32_t offset = 4 + index * 4;
    frame_buf[offset + 0] = 0xE0 | brightness;
    frame_buf[offset + 1] = b;
    frame_buf[offset + 2] = g;
    frame_buf[offset + 3] = r;
}

void sk9822_led::update()
{
    spi_send(frame_buf, FRAME_SIZE);
}

void sk9822_led::clear()
{
    // Start frame: 32 bits of 0
    frame_buf[0] = 0x00;
    frame_buf[1] = 0x00;
    frame_buf[2] = 0x00;
    frame_buf[3] = 0x00;

    // All LEDs off (brightness=0, BGR=0)
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        uint32_t offset = 4 + i * 4;
        frame_buf[offset + 0] = 0xE0; // brightness = 0
        frame_buf[offset + 1] = 0x00;
        frame_buf[offset + 2] = 0x00;
        frame_buf[offset + 3] = 0x00;
    }

    // End frame: 32 bits of 1
    uint32_t end_offset = 4 + NUM_LEDS * 4;
    frame_buf[end_offset + 0] = 0xFF;
    frame_buf[end_offset + 1] = 0xFF;
    frame_buf[end_offset + 2] = 0xFF;
    frame_buf[end_offset + 3] = 0xFF;
}

} // namespace sed_ws63
