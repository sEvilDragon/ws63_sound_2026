#include "spi_master.hpp"

namespace sed_ws63 {

spi_master::spi_master()
{
    pin_init();
    spi_init();
    osal_printk("[SPI_Master] SPI_BUS_1 Master 初始化完成\r\n");
}

void spi_master::pin_init()
{
    // 接收端 SPI_BUS_1 (Master) 引脚: GPIO_00(CSN), GPIO_01(MOSI), GPIO_03(MISO), GPIO_06(SCK)
    // 全部复用 PIN_MODE_3
    uapi_pin_set_mode(GPIO_00, PIN_MODE_3); // CSN / QSPI0_CSN
    uapi_pin_set_mode(GPIO_01, PIN_MODE_3); // MOSI / QSPI0_IO0
    uapi_pin_set_mode(GPIO_03, PIN_MODE_3); // MISO / QSPI0_IO1
    uapi_pin_set_mode(GPIO_06, PIN_MODE_3); // SCK / QSPI0_SCK
}

void spi_master::spi_init()
{
    errcode_t ret;

    // 基本属性配置
    spi_attr_t config = {};
    config.is_slave = false;                                 // 主模式
    config.slave_num = SPI_SLAVE0;                           // 选择从机 0
    config.bus_clk = 32000000;                               // 总线时钟 32MHz
    config.freq_mhz = FREQ_MHZ;                              // 2MHz 工作频率
    config.clk_polarity = SPI_CFG_CLK_CPOL_0;                // CPOL = 0
    config.clk_phase = SPI_CFG_CLK_CPHA_0;                   // CPHA = 0
    config.frame_format = SPI_CFG_FRAME_FORMAT_MOTOROLA_SPI; // Motorola SPI
    config.spi_frame_format = HAL_SPI_FRAME_FORMAT_STANDARD; // 标准单线
    config.frame_size = HAL_SPI_FRAME_SIZE_8;                // 8-bit
    config.tmod = HAL_SPI_TRANS_MODE_TXRX;                   // 全双工
    config.sste = SPI_CFG_SSTE_ENABLE;                       // 帧间 CS 翻转
    config.ndf = 0;                                          // 不控制帧数

    // 扩展属性: 轮询模式(不启用 DMA/中断)
    spi_extra_attr_t extra = {};

    ret = uapi_spi_init(BUS, &config, &extra);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SPI_Master] SPI_BUS_1 初始化失败, ret=0x%x\r\n", ret);
        return;
    }

    // 选择从机 0
    ret = uapi_spi_select_slave(BUS, SPI_SLAVE0);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SPI_Master] 选择从机失败, ret=0x%x\r\n", ret);
    }
}

int spi_master::send(const uint8_t *data, uint8_t len)
{
    if (data == nullptr && len > 0)
        return -1;
    if (len > MAX_PAYLOAD)
        return -1;

    // 构造帧: [0xA5][len][data...]
    uint8_t tx_buffer[MAX_FRAME_SIZE];
    tx_buffer[0] = FRAME_HEADER;
    tx_buffer[1] = len;
    if (len > 0) {
        for (uint8_t i = 0; i < len; i++) {
            tx_buffer[FRAME_HDR_SIZE + i] = data[i];
        }
    }

    spi_xfer_data_t xfer = {};
    xfer.tx_buff = tx_buffer;
    xfer.tx_bytes = FRAME_HDR_SIZE + len;
    xfer.rx_buff = nullptr;
    xfer.rx_bytes = 0;

    errcode_t ret = uapi_spi_master_write(BUS, &xfer, TIMEOUT);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SPI_Master] 发送失败, ret=0x%x\r\n", ret);
        return -1;
    }
    return 0;
}

int spi_master::recv(uint8_t *data, uint8_t max_len)
{
    if (data == nullptr || max_len == 0)
        return -1;

    // 先读取帧头(2 字节)
    uint8_t header[FRAME_HDR_SIZE] = {0};
    spi_xfer_data_t xfer = {};
    xfer.tx_buff = nullptr;
    xfer.tx_bytes = 0;
    xfer.rx_buff = header;
    xfer.rx_bytes = FRAME_HDR_SIZE;

    errcode_t ret = uapi_spi_master_read(BUS, &xfer, TIMEOUT);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SPI_Master] 读取帧头失败, ret=0x%x\r\n", ret);
        return -1;
    }

    // 校验帧头
    if (header[0] != FRAME_HEADER) {
        osal_printk("[SPI_Master] 帧头错误: 0x%02X (期望 0xA5)\r\n", header[0]);
        return -1;
    }

    uint8_t payload_len = header[1];
    if (payload_len == 0) {
        return 0; // 空帧
    }
    if (payload_len > max_len) {
        osal_printk("[SPI_Master] 载荷过长: %u > %u\r\n", payload_len, max_len);
        return -1;
    }

    // 读取载荷
    xfer.rx_buff = data;
    xfer.rx_bytes = payload_len;
    ret = uapi_spi_master_read(BUS, &xfer, TIMEOUT);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SPI_Master] 读取载荷失败, ret=0x%x\r\n", ret);
        return -1;
    }

    return payload_len;
}

int spi_master::writeread(const uint8_t *tx_data, uint8_t tx_len, uint8_t *rx_data, uint8_t rx_max)
{
    if ((tx_data == nullptr && tx_len > 0) || (rx_data == nullptr && rx_max > 0))
        return -1;
    if (tx_len > MAX_PAYLOAD)
        return -1;

    // 构造发送帧
    uint8_t tx_buffer[MAX_FRAME_SIZE];
    tx_buffer[0] = FRAME_HEADER;
    tx_buffer[1] = tx_len;
    if (tx_len > 0) {
        for (uint8_t i = 0; i < tx_len; i++) {
            tx_buffer[FRAME_HDR_SIZE + i] = tx_data[i];
        }
    }
    uint16_t tx_total = FRAME_HDR_SIZE + tx_len;

    // 接收缓冲区(最多接收帧头 + 最大载荷)
    uint8_t rx_buffer[MAX_FRAME_SIZE] = {0};
    uint16_t rx_total = (rx_max > 0) ? MAX_FRAME_SIZE : 0;

    spi_xfer_data_t xfer = {};
    xfer.tx_buff = tx_buffer;
    xfer.tx_bytes = tx_total;
    xfer.rx_buff = rx_buffer;
    xfer.rx_bytes = rx_total;

    errcode_t ret = uapi_spi_master_writeread(BUS, &xfer, TIMEOUT);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SPI_Master] 全双工收发失败, ret=0x%x\r\n", ret);
        return -1;
    }

    // 解析接收帧
    if (rx_max == 0)
        return 0; // 未请求接收

    if (rx_buffer[0] != FRAME_HEADER) {
        osal_printk("[SPI_Master] 接收帧头错误: 0x%02X\r\n", rx_buffer[0]);
        return -1;
    }

    uint8_t rx_len = rx_buffer[1];
    if (rx_len == 0)
        return 0;
    if (rx_len > rx_max) {
        osal_printk("[SPI_Master] 接收载荷过长: %u > %u\r\n", rx_len, rx_max);
        return -1;
    }

    for (uint8_t i = 0; i < rx_len; i++) {
        rx_data[i] = rx_buffer[FRAME_HDR_SIZE + i];
    }
    return rx_len;
}

} // namespace sed_ws63
