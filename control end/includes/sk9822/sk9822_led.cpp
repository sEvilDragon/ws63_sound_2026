#include "sk9822_led.hpp"

namespace sed_ws63 {

uint8_t sk9822_led::frame_buf[sk9822_led::FRAME_SIZE];

sk9822_led::sk9822_led()
{
    osal_printk("[SK9822] ===== ctor START (BUS_1, DMA per-transfer) =====\r\n");
    osal_printk("[SK9822] ctor: step1 pin_init...\r\n");
    pin_init();
    osal_printk("[SK9822] ctor: step2 spi_init...\r\n");
    spi_init();
    osal_printk("[SK9822] ctor: step3 spi_dma_init (empty)...\r\n");
    spi_dma_init();
    osal_printk("[SK9822] ctor: step4 clear (no SPI)...\r\n");
    clear();
    osal_printk("[SK9822] ===== ctor DONE, %d LEDs =====\r\n", NUM_LEDS);
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

    osal_printk("[SK9822] calling uapi_spi_init(BUS_1, master, freq=%uMHz)...\r\n", (unsigned)FREQ_MHZ);
    errcode_t ret = uapi_spi_init(BUS, &config, &ext_config);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SK9822] *** FATAL: SPI init failed! ret=0x%X ***\r\n", (unsigned)ret);
    } else {
        osal_printk("[SK9822] uapi_spi_init OK\r\n");
    }
}

void sk9822_led::spi_dma_init()
{
    /* 不在此处启用 DMA!
     * spi_write_dma() 传输完成后不会关闭 QSPI 的 DMA 请求,
     * 导致 TX FIFO 空时产生孤儿 DMA 请求, 卡死 DMA 控制器,
     * 进而阻塞 spi_slave (SPI_BUS_0) 的 DMA 通道。
     * 改为在 spi_send() 中临时启用/关闭 DMA, 传输完立即释放。 */
}

void sk9822_led::spi_send(const uint8_t *data, uint32_t len)
{
    static int send_cnt = 0;
    send_cnt++;

    spi_xfer_data_t xfer = {0};
    xfer.tx_buff = (uint8_t *)data;
    xfer.tx_bytes = len;

    /* 每次传输前切换到 Master 模式 (WS63 全局只有 1 个模式 bit, 分时复用) */
    spi_porting_set_device_mode(BUS, SPI_MODE_MASTER);
    spi_dma_config_t dma_cfg = {.src_width = 0, .dest_width = 0, .burst_length = 0, .priority = 0};
    errcode_t dma_ret = uapi_spi_set_dma_mode(BUS, true, &dma_cfg);

    // 前 5 次 + 每 50 次打印一次, 确认持续在工作
    if (send_cnt <= 5 || send_cnt % 50 == 0) {
        osal_printk("[SK9822] send #%d: DMA en ret=0x%X, tx_bytes=%u\r\n",
                    send_cnt, (unsigned)dma_ret, (unsigned)len);
    }

    errcode_t ret = uapi_spi_master_write(BUS, &xfer, TIMEOUT);

    errcode_t dma_dis_ret = uapi_spi_set_dma_mode(BUS, false, NULL);

    if (ret != ERRCODE_SUCC) {
        osal_printk("[SK9822] *** send #%d FAIL: write ret=0x%X (dma_en=0x%X dma_dis=0x%X) ***\r\n",
                    send_cnt, (unsigned)ret, (unsigned)dma_ret, (unsigned)dma_dis_ret);
    } else if (send_cnt <= 5 || send_cnt % 50 == 0) {
        osal_printk("[SK9822] send #%d OK (dma_en=0x%X dma_dis=0x%X)\r\n",
                    send_cnt, (unsigned)dma_ret, (unsigned)dma_dis_ret);
    }
}

void sk9822_led::set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    if (index >= NUM_LEDS)
        return;
    if (brightness > 31)
        brightness = 31;

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
