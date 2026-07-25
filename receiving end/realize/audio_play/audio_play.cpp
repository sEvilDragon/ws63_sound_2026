#include "audio_play.hpp"
#include "../../../other/adpcm/adpcm.hpp"
#include "spi_task.h"
#include "wifi_task.hpp"

#include <cstring>

extern "C" {
#include "soc_osal.h"
}

static volatile bool s_sle_stop = false;
static volatile bool s_sle_done = false;
static volatile bool s_sle_running = false;
static osal_task *s_sle_task_handle = nullptr;
static constexpr int k_sle_stop_timeout_loops = 80;
static constexpr int k_sle_release_grace_ms = 200;
static constexpr std::size_t k_max_sle_pcm_samples = 960; /* interleaved stereo samples */
static constexpr std::size_t k_max_sle_mono_samples = k_max_sle_pcm_samples / 2U;
static adpcm s_adpcm_decoder;
static int16_t s_sle_pcm_buffer[k_max_sle_pcm_samples] = {0};
static int16_t s_sle_mono_buffer[k_max_sle_mono_samples] = {0};

static std::size_t expand_mono_to_stereo(const int16_t *mono, std::size_t samples, int16_t *stereo,
                                         std::size_t stereo_capacity)
{
    if (mono == nullptr || stereo == nullptr || samples == 0 || samples > stereo_capacity / 2U) {
        return 0;
    }
    for (std::size_t i = 0; i < samples; ++i) {
        stereo[2U * i] = mono[i];
        stereo[2U * i + 1U] = mono[i];
    }
    return samples * 2U;
}

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
    if (data == nullptr || len < 1) {
        return;
    }

    const char *format = nullptr;
    std::size_t pcm_samples = 0;
    if (data[0] == sle_audio::adpcm_mono_packet_marker) {
        format = "ADPCM mono";
        const std::size_t mono_samples =
            s_adpcm_decoder.decode_mono(data, len, s_sle_mono_buffer, k_max_sle_mono_samples);
        pcm_samples = expand_mono_to_stereo(s_sle_mono_buffer, mono_samples, s_sle_pcm_buffer,
                                            k_max_sle_pcm_samples);
        if (pcm_samples == 0) {
            osal_printk("[Audio] invalid mono ADPCM packet: len=%u\r\n", len);
            return;
        }
    } else if (sle_audio::is_adpcm_packet(data[0])) {
        format = "ADPCM stereo";
        pcm_samples = s_adpcm_decoder.decode(data, len, s_sle_pcm_buffer, k_max_sle_pcm_samples);
        if (pcm_samples == 0) {
            osal_printk("[Audio] invalid ADPCM packet: marker=0x%02x len=%u\r\n", data[0], len);
            return;
        }
    } else if (data[0] == sle_audio::pcm_mono_packet_marker) {
        format = "PCM mono";
        const std::size_t payload_bytes = len - 1U;
        if ((payload_bytes & 1U) != 0 || payload_bytes > sizeof(s_sle_mono_buffer)) {
            return;
        }
        std::memcpy(s_sle_mono_buffer, data + 1, payload_bytes);
        pcm_samples = expand_mono_to_stereo(s_sle_mono_buffer, payload_bytes / sizeof(int16_t), s_sle_pcm_buffer,
                                            k_max_sle_pcm_samples);
    } else if (data[0] == sle_audio::pcm_packet_marker) {
        format = "PCM stereo";
        const std::size_t payload_bytes = len - 1U;
        if ((payload_bytes & 1U) != 0 || payload_bytes > sizeof(s_sle_pcm_buffer)) {
            return;
        }
        std::memcpy(s_sle_pcm_buffer, data + 1, payload_bytes);
        pcm_samples = payload_bytes / sizeof(int16_t);
    } else {
        /* Unknown/legacy packets have no reliable codec marker. */
        return;
    }

    if (pcm_samples == 0) {
        return;
    }

    static uint8_t last_marker = 0;
    if (data[0] != last_marker) {
        osal_printk("[Audio] SLE RX format: %s, marker=0x%02x len=%u\r\n", format, data[0], len);
        last_marker = data[0];
    }

    const spi_settings_t *s = get_spi_settings();
    iis::data_write(s_sle_pcm_buffer, pcm_samples, spi_settings_effective_volume(s),
                    spi_settings_effective_bass(s));
    iis::fill_buffer_if_needed();
}

static void *sle_audio_task(void *arg)
{
    (void)arg;

    sle::set_data_process_fuction(sle_data_process);
    sle::set_data_clear_fuction(iis::data_clear);
    osal_printk("[Audio] initializing SLE mode\r\n");
    sle g_sle;

    while (!s_sle_stop) {
        osal_msleep(100);
    }

    sle::teardown();
    iis::data_clear();
    s_sle_done = true;
    osal_printk("[Audio] SLE mode stopped\r\n");
    return nullptr;
}

static void start_sle_mode(void)
{
    s_sle_stop = false;
    s_sle_done = false;
    s_sle_task_handle = osal_kthread_create((osal_kthread_handler)sle_audio_task, NULL, "sle_audio", 4096);
    s_sle_running = (s_sle_task_handle != nullptr);
    if (!s_sle_running) {
        osal_printk("[Audio] SLE task creation failed\r\n");
    }
}

static bool stop_sle_mode(void)
{
    if (!s_sle_task_handle) {
        s_sle_running = false;
        return true;
    }
    s_sle_stop = true;
    int timeout = k_sle_stop_timeout_loops;
    while (!s_sle_done && timeout-- > 0) {
        osal_msleep(100);
    }
    if (!s_sle_done) {
        osal_printk("[Audio] SLE task did not stop within %d ms\r\n",
                    k_sle_stop_timeout_loops * 100);
        return false;
    }

    // Give the task a short scheduling window after its teardown returns
    // before another audio mode is allowed to allocate resources.
    osal_msleep(k_sle_release_grace_ms);
    s_sle_task_handle = nullptr;
    s_sle_running = false;
    return true;
}

bool audio_sle_task_running(void)
{
    return s_sle_running;
}

void *audio_play_task(void *arg)
{
    (void)arg;

    static iis g_iis;

    uint8_t current_mode = get_spi_settings()->mode;

    // 根据初始模式设置 IIS 采样率：SLE 用 48kHz，DLNA 用 44.1kHz
    if (current_mode == SPI_MODE_SLE || current_mode == SPI_MODE_SLE_MIC) {
        iis::set_rate_of_iis(I2S_SAMPLE_RATE_48K);
        start_sle_mode();
    } else if (current_mode == SPI_MODE_DLNA || current_mode == SPI_MODE_DLNA_NET) {
        iis::set_rate_of_iis(I2S_SAMPLE_RATE_44K);
    }

    while (true) {
        uint8_t new_mode = get_spi_settings()->mode;
        if (new_mode != current_mode) {
            if ((new_mode == SPI_MODE_SLE || new_mode == SPI_MODE_SLE_MIC) && wifi_dlna_tasks_running()) {
                // Wait for wifi_task's DLNA shutdown grace period before
                // allocating the SLE task and its audio resources.
                osal_msleep(100);
                continue;
            }
            osal_printk("[Audio] mode switch: %s -> %s\r\n", mode_name(current_mode), mode_name(new_mode));

            if (current_mode == SPI_MODE_SLE || current_mode == SPI_MODE_SLE_MIC) {
                if (!stop_sle_mode()) {
                    osal_msleep(100);
                    continue;
                }
            }

            iis::data_clear();

            // 根据新模式切换 IIS 采样率：SLE → 48kHz，DLNA → 44.1kHz
            if (new_mode == SPI_MODE_SLE || new_mode == SPI_MODE_SLE_MIC) {
                iis::set_rate_of_iis(I2S_SAMPLE_RATE_48K);
                start_sle_mode();
            } else if (new_mode == SPI_MODE_DLNA || new_mode == SPI_MODE_DLNA_NET) {
                iis::set_rate_of_iis(I2S_SAMPLE_RATE_44K);
            }

            current_mode = new_mode;
        }
        osal_msleep(100);
    }
}
