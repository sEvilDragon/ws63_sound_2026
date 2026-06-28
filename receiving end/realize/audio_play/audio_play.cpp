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
        case SPI_MODE_WIREED:
            return "WIRED";
        case SPI_MODE_SLE:
            return "SLE";
        case SPI_MODE_DLNA:
            return "DLNA";
        case SPI_MODE_SLE_MIC:
            return "SLE_MIC";
        case SPI_MODE_DLNA_NET:
            return "DLNA_NET";
        default:
            return "UNKNOWN";
    }
}

static void sle_data_process(const uint8_t *data, uint16_t len)
{
    static int sdp_cnt = 0;
    sdp_cnt++;
    if (data == nullptr) {
        if (sdp_cnt <= 3)
            osal_printk("[SLE] data_process #%d: NULL data, drop!\r\n", sdp_cnt);
        return;
    }
    if (len % 2 == 1) {
        if (sdp_cnt <= 3)
            osal_printk("[SLE] data_process #%d: odd len=%u, drop!\r\n", sdp_cnt, len);
        return;
    }
    if (sdp_cnt <= 3 || sdp_cnt % 100 == 1) {
        osal_printk("[SLE] data_process #%d: len=%u -> data_write samples=%u\r\n", sdp_cnt, len, len / sizeof(int16_t));
    }
    iis::data_write((const int16_t *)data, len / sizeof(int16_t), get_spi_settings()->volume, get_spi_settings()->bass);
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
    s_sle_task_handle = osal_kthread_create((osal_kthread_handler)sle_audio_task, NULL, "sle_audio", 4096);
    osal_printk("[Mode] SLE task created\r\n");
}

static void stop_sle_mode(void)
{
    if (!s_sle_task_handle)
        return;
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

    // 根据初始模式设置 IIS 采样率：SLE 用 48kHz，DLNA 用 44.1kHz
    if (current_mode == SPI_MODE_SLE || current_mode == SPI_MODE_SLE_MIC) {
        iis::set_rate_of_iis(I2S_SAMPLE_RATE_48K);
        osal_printk("[Audio] IIS rate set to 48K for SLE mode\r\n");
        start_sle_mode();
    } else if (current_mode == SPI_MODE_DLNA || current_mode == SPI_MODE_DLNA_NET) {
        iis::set_rate_of_iis(I2S_SAMPLE_RATE_44K);
        osal_printk("[Audio] IIS rate set to 44.1K for DLNA mode\r\n");
    }

    while (true) {
        uint8_t new_mode = get_spi_settings()->mode;
        if (new_mode != current_mode) {
            osal_printk("[Audio] mode switch: %s -> %s\r\n", mode_name(current_mode), mode_name(new_mode));

            if (current_mode == SPI_MODE_SLE || current_mode == SPI_MODE_SLE_MIC) {
                stop_sle_mode();
            }

            osal_printk("[Audio] calling data_clear for mode switch: %s -> %s\r\n", mode_name(current_mode),
                        mode_name(new_mode));
            iis::data_clear();

            // 根据新模式切换 IIS 采样率：SLE → 48kHz，DLNA → 44.1kHz
            if (new_mode == SPI_MODE_SLE || new_mode == SPI_MODE_SLE_MIC) {
                iis::set_rate_of_iis(I2S_SAMPLE_RATE_48K);
                osal_printk("[Audio] IIS rate switched to 48K for SLE mode\r\n");
                start_sle_mode();
            } else if (new_mode == SPI_MODE_DLNA || new_mode == SPI_MODE_DLNA_NET) {
                iis::set_rate_of_iis(I2S_SAMPLE_RATE_44K);
                osal_printk("[Audio] IIS rate switched to 44.1K for DLNA mode\r\n");
            }

            current_mode = new_mode;
        }
        osal_msleep(100);
    }
}
