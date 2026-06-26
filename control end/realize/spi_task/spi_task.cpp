#include "spi_task.h"
#include "spi_slave.hpp"

static spi_settings_t g_settings = {
    SPI_CMD_QUERY, (uint8_t)((SPI_HOTSPOT_OFF << 4) | SPI_NETWORK_CONN), SPI_MODE_WIREED, 25, 50, 0};

const spi_settings_t *get_spi_settings()
{
    return &g_settings;
}

void *spi_slave_task(void *arg)
{
    (void)arg;
    static sed_ws63::spi_slave spi;
    osal_printk("[SPI_Slave] settings task started\r\n");

    uint8_t rx_buf[sed_ws63::spi_slave::TRANSFER_LEN];

    while (true) {
        spi_settings_t response = g_settings;
        response.cmd = SPI_CMD_QUERY;

        spi.transfer(rx_buf, sed_ws63::spi_slave::TRANSFER_LEN, (const uint8_t *)&response, SPI_SETTINGS_LEN);
        (void)rx_buf;
        // control end is authoritative: ignore master data,
        // only send our settings back (master reads on QUERY cycle)

        osal_msleep(500); // 与 Master 同步周期, 防止 FIFO 堆积
    }
    return nullptr;
}
