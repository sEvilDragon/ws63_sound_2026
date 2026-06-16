#include "spi_task.h"
#include "spi_slave.hpp"

static spi_settings_t g_settings = {
    SPI_CMD_QUERY, (uint8_t)((SPI_HOTSPOT_OFF << 4) | SPI_NETWORK_CONN), SPI_MODE_WIREED, 50, 50, 50};

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

        int ret = spi.transfer(rx_buf, sed_ws63::spi_slave::TRANSFER_LEN, (const uint8_t *)&response, SPI_SETTINGS_LEN);

        if (ret == 0) {
            spi_settings_t received;
            for (int i = 0; i < SPI_SETTINGS_LEN; i++) {
                ((uint8_t *)&received)[i] = rx_buf[i];
            }

            if (spi_validate_settings(&received)) {
                if (received.cmd == SPI_CMD_SYNC) {
                    g_settings.hotspot_network = received.hotspot_network;
                    g_settings.mode = received.mode;
                    g_settings.volume = received.volume;
                    g_settings.brightness = received.brightness;
                    g_settings.bass = received.bass;
                }
            }
        }

        osal_msleep(500); // 与 Master 同步周期, 防止 FIFO 堆积
    }
    return nullptr;
}
