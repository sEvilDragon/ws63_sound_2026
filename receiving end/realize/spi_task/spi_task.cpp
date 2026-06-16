#include "spi_task.h"
#include "spi_master.hpp"

static spi_settings_t g_settings = {
    SPI_CMD_SYNC,
    (uint8_t)((SPI_HOTSPOT_OFF << 4) | SPI_NETWORK_CONN),
    SPI_MODE_WIREED,
    25, 50, 0
};

const spi_settings_t *get_spi_settings()
{
    return &g_settings;
}

void spi_settings_update_hotspot_network(uint8_t hotspot, uint8_t network)
{
    uint8_t packed = spi_make_hotspot_network(hotspot, network);
    if (spi_validate_hotspot_network(packed)) {
        g_settings.hotspot_network = packed;
        g_settings.cmd = SPI_CMD_SYNC;
    }
}

void spi_settings_update_mode(uint8_t mode)
{
    if (spi_validate_mode(mode)) {
        g_settings.mode = mode;
        g_settings.cmd = SPI_CMD_SYNC;
    }
}

void spi_settings_update_volume(uint8_t volume)
{
    if (spi_validate_percent(volume)) {
        g_settings.volume = volume;
        g_settings.cmd = SPI_CMD_SYNC;
    }
}

void spi_settings_update_brightness(uint8_t brightness)
{
    if (spi_validate_percent(brightness)) {
        g_settings.brightness = brightness;
        g_settings.cmd = SPI_CMD_SYNC;
    }
}

void spi_settings_update_bass(uint8_t bass)
{
    if (spi_validate_percent(bass)) {
        g_settings.bass = bass;
        g_settings.cmd = SPI_CMD_SYNC;
    }
}

void *spi_master_task(void *arg)
{
    (void)arg;
    static sed_ws63::spi_master spi;
    osal_printk("[SPI_Master] settings task started\r\n");

    uint8_t rx_buf[sed_ws63::spi_master::TRANSFER_LEN];

    while (true) {
        int ret = spi.transfer((const uint8_t *)&g_settings, SPI_SETTINGS_LEN,
                               rx_buf, sed_ws63::spi_master::TRANSFER_LEN);

        if (ret == 0) {
            spi_settings_t resp;
            for (int i = 0; i < SPI_SETTINGS_LEN; i++) {
                ((uint8_t *)&resp)[i] = rx_buf[i];
            }

            if (spi_validate_settings(&resp)) {
                if (g_settings.cmd == SPI_CMD_QUERY) {
                    g_settings.hotspot_network = resp.hotspot_network;
                    g_settings.mode = resp.mode;
                    g_settings.volume = resp.volume;
                    g_settings.brightness = resp.brightness;
                    g_settings.bass = resp.bass;
                }
            }
        }

        if (g_settings.cmd == SPI_CMD_SYNC) {
            g_settings.cmd = SPI_CMD_QUERY;
        }

        osal_msleep(500);
    }
    return nullptr;
}
