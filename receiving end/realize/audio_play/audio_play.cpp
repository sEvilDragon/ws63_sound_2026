#include "audio_play.hpp"
#include "spi_task.h"

extern "C" {
#include "soc_osal.h"
}

static volatile bool s_sle_stop = false;
static volatile bool s_sle_done = false;
static osal_task *s_sle_task_handle = nullptr;

static const char *mode_name(uint8_t mode)
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
    if (data == nullptr || len % 2 == 1)
        return;
    iis::data_write((const int16_t *)data, len / sizeof(int16_t),
                     get_spi_settings()->volume, get_spi_settings()->bass);
    iis::fill_buffer_if_needed();
}

static void *sle_audio_task(void *arg)
{
    (void)arg;
    osal_printk("[SLE] task started\r\n");

    sle::set_data_process_fuction(sle_data_process);
    sle::set_data_clear_fuction(iis::data_clear);
    static sle g_sle;

    while (!s_sle_stop) {
        osal_msleep(100);
    }

    osal_printk("[SLE] stopping...\r\n");
    sle::teardown();
    iis::data_clear();
    s_sle_done = true;
    osal_printk("[SLE] task exited\r\n");
    return nullptr;
}

static void start_sle_mode(void)
{
    s_sle_stop = false;
    s_sle_done = false;
    s_sle_task_handle = osal_kthread_create(
        (osal_kthread_handler)sle_audio_task, NULL, "sle_audio", 4096);
    osal_printk("[Mode] SLE task created\r\n");
}

static void stop_sle_mode(void)
{
    if (!s_sle_task_handle) return;
    s_sle_stop = true;
    int timeout = 50;
    while (!s_sle_done && timeout-- > 0) {
        osal_msleep(100);
    }
    s_sle_task_handle = nullptr;
    osal_printk("[Mode] SLE stopped\r\n");
}

void *audio_play_task(void *arg)
{
    (void)arg;

    static iis g_iis;
    osal_printk("[Audio] IIS ready\r\n");

    uint8_t current_mode = get_spi_settings()->mode;
    osal_printk("[Audio] initial mode: %s\r\n", mode_name(current_mode));

    if (current_mode == SPI_MODE_SLE || current_mode == SPI_MODE_SLE_MIC) {
        start_sle_mode();
    }

    while (true) {
        uint8_t new_mode = get_spi_settings()->mode;
        if (new_mode != current_mode) {
            osal_printk("[Audio] mode switch: %s -> %s\r\n",
                        mode_name(current_mode), mode_name(new_mode));

            if (current_mode == SPI_MODE_SLE || current_mode == SPI_MODE_SLE_MIC) {
                stop_sle_mode();
            }

            osal_printk("[Audio] calling data_clear for mode switch: %s -> %s\r\n",
                        mode_name(current_mode), mode_name(new_mode));
            iis::data_clear();

            if (new_mode == SPI_MODE_SLE || new_mode == SPI_MODE_SLE_MIC) {
                start_sle_mode();
            }

            current_mode = new_mode;
        }
        osal_msleep(100);
    }
}
