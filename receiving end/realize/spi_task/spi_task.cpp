#include "spi_task.h"
#include "dws_master.hpp"

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

static spi_settings_t g_settings = {
    SPI_CMD_SYNC, (uint8_t)((SPI_HOTSPOT_OFF << 4) | SPI_NETWORK_CONN), SPI_MODE_WIREED, 25, 50, 0};

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
    static sed_ws63::dws_master spi;

    uint8_t rx_buf[sed_ws63::dws_master::TRANSFER_LEN];
    uint8_t prev_mode = g_settings.mode;
    uint8_t prev_volume = g_settings.volume;
    uint8_t prev_bass = g_settings.bass;
    uint32_t dbg_tick = 0;

    while (true) {
        audio_analyzer::compute();
        const audio_result_t &audio = audio_analyzer::get_result();

        /* 每 40 帧 (~2s) 输出一次音频数据，定位噪声来源 */
        dbg_tick++;
        if (dbg_tick % 40 == 0) {
            osal_printk("[AUDIO] bands=[%3u %3u %3u %3u %3u] ov=%3u beat=%u\r\n",
                (unsigned)audio.bands[0], (unsigned)audio.bands[1],
                (unsigned)audio.bands[2], (unsigned)audio.bands[3],
                (unsigned)audio.bands[4], (unsigned)audio.overall,
                (unsigned)audio.beat);
        }

        uint8_t tx_buf[sed_ws63::dws_master::TRANSFER_LEN] = {0};
        const uint8_t *settings_bytes = (const uint8_t *)&g_settings;
        for (int i = 0; i < SPI_SETTINGS_LEN; i++) {
            tx_buf[i] = settings_bytes[i];
        }
        /* 记录本帧实际发送的 cmd, 防止 transfer 期间被外部改掉后误清除 SYNC */
        uint8_t tx_cmd_snapshot = g_settings.cmd;
        tx_buf[SPI_AUDIO_OFFSET + 0] = audio.bands[0];
        tx_buf[SPI_AUDIO_OFFSET + 1] = audio.bands[1];
        tx_buf[SPI_AUDIO_OFFSET + 2] = audio.bands[2];
        tx_buf[SPI_AUDIO_OFFSET + 3] = audio.bands[3];
        tx_buf[SPI_AUDIO_OFFSET + 4] = audio.bands[4];
        tx_buf[SPI_AUDIO_OFFSET + 5] = audio.overall;
        /* beat 仅 0/1, 校验后发出以防对端收到错位数据 */
        tx_buf[SPI_AUDIO_OFFSET + 6] = (audio.beat != 0) ? (uint8_t)1 : (uint8_t)0;

        int ret = spi.transfer(tx_buf, sed_ws63::dws_master::TRANSFER_LEN, rx_buf, sed_ws63::dws_master::TRANSFER_LEN);

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

        if (tx_cmd_snapshot == SPI_CMD_SYNC) {
            g_settings.cmd = SPI_CMD_QUERY;
        }

        if (g_settings.mode != prev_mode) {
            osal_printk("[DWS_M] MODE CHANGED: %s -> %s\r\n", mode_name(prev_mode), mode_name(g_settings.mode));
            prev_mode = g_settings.mode;
        }
        if (g_settings.volume != prev_volume) {
            osal_printk("[DWS_M] volume: %u -> %u\r\n", (unsigned)prev_volume, (unsigned)g_settings.volume);
            prev_volume = g_settings.volume;
        }
        if (g_settings.bass != prev_bass) {
            osal_printk("[DWS_M] bass: %u -> %u\r\n", (unsigned)prev_bass, (unsigned)g_settings.bass);
            prev_bass = g_settings.bass;
        }

        osal_msleep(50);
    }
    return nullptr;
}
