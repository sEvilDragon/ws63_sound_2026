#include "audio_play.hpp"
#include "spi_task.h"

static const char *audio_mode_name(uint8_t mode)
{
    switch (mode) {
        case SPI_MODE_WIREED:   return "WIRED";
        case SPI_MODE_SLE:      return "SLE";
        case SPI_MODE_DLNA:     return "DLNA";
        case SPI_MODE_SLE_MIC:  return "SLE_MIC";
        case SPI_MODE_DLNA_NET: return "DLNA_NET";
        default:                return "UNKNOWN";
    }
}

static void sle_data_process(const uint8_t *data, uint16_t len)
{
    if (get_spi_settings()->mode != SPI_MODE_SLE)
        return;
    if (data == nullptr || len % 2 == 1)
        return;

    iis::data_write((const int16_t *)data, len / sizeof(int16_t), get_spi_settings()->volume, get_spi_settings()->bass);
    iis::fill_buffer_if_needed();
}

void *audio_play_task(void *arg)
{
    (void)arg;
    static iis g_iis;
    osal_printk("[Audio] IIS ready\r\n");

    sle::set_data_process_fuction(sle_data_process);
    sle::set_data_clear_fuction(iis::data_clear);

    static sle g_sle;
    osal_printk("[Audio] SLE ready, entering main loop\r\n");

    uint8_t last_mode = get_spi_settings()->mode;
    osal_printk("[Audio] current mode: %s\r\n", audio_mode_name(last_mode));

    while (true) {
        uint8_t cur_mode = get_spi_settings()->mode;
        if (cur_mode != last_mode) {
            osal_printk("[Audio] mode switch: %s -> %s\r\n",
                        audio_mode_name(last_mode), audio_mode_name(cur_mode));
            iis::data_clear();
            last_mode = cur_mode;
        }
        osal_msleep(100);
    }
}
