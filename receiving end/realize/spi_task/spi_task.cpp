#include "spi_task.h"
#include "dws_master.hpp"

extern "C" {
#include "nv.h"
#include "systick.h"
}

#define NV_KEY_SPI_SETTINGS 0x5001
#define NV_FLUSH_DELAY_MS 5000

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
    SPI_CMD_QUERY, (uint8_t)((SPI_HOTSPOT_OFF << 4) | SPI_NETWORK_CONN), SPI_MODE_WIREED, 25, 50, 0,
    SPI_TONE_FLAT, 0};

static bool g_nv_dirty = false;
static uint64_t g_nv_last_change_tick = 0;

static bool spi_settings_payload_equal(const spi_settings_t *a, const spi_settings_t *b)
{
    return a->hotspot_network == b->hotspot_network &&
           a->mode == b->mode &&
           a->volume == b->volume &&
           a->brightness == b->brightness &&
           a->bass == b->bass &&
           a->tone == b->tone &&
           a->flags == b->flags;
}

static bool spi_settings_recv_nv_payload_equal(const spi_settings_t *a, const spi_settings_t *b)
{
    return a->hotspot_network == b->hotspot_network &&
           a->mode == b->mode &&
           a->volume == b->volume &&
           a->brightness == b->brightness &&
           a->bass == b->bass;
}

const spi_settings_t *get_spi_settings()
{
    return &g_settings;
}

static void spi_settings_mark_dirty(void)
{
    g_nv_dirty = true;
    g_nv_last_change_tick = uapi_systick_get_ms();
}

static void spi_settings_mark_sync(bool persist_recv_nv)
{
    g_settings.cmd = SPI_CMD_SYNC;
    if (persist_recv_nv) {
        spi_settings_mark_dirty();
    }
}

static void spi_settings_apply_control_payload(const spi_settings_t *src)
{
    if (src == nullptr || !spi_validate_settings(src)) {
        return;
    }

    bool persist_changed = !spi_settings_recv_nv_payload_equal(&g_settings, src);
    bool flags_changed = g_settings.flags != src->flags;
    if (!persist_changed && !flags_changed) {
        return;
    }

    g_settings.hotspot_network = src->hotspot_network;
    g_settings.mode = src->mode;
    g_settings.volume = src->volume;
    g_settings.brightness = src->brightness;
    g_settings.bass = src->bass;
    g_settings.flags = src->flags;
    if (persist_changed) {
        spi_settings_mark_dirty();
    }
}

int spi_settings_load_from_nv(void)
{
    uint16_t len = 0;
    spi_settings_t saved = g_settings;
    errcode_t ret = uapi_nv_read(NV_KEY_SPI_SETTINGS, sizeof(saved), &len, (uint8_t *)&saved);
    if (ret == ERRCODE_SUCC && (len == sizeof(saved) || len == SPI_SETTINGS_LEGACY_LEN) &&
        spi_validate_settings(&saved)) {
        g_settings = saved;
        g_settings.cmd = SPI_CMD_QUERY;
        g_settings.flags = 0;
        osal_printk("[DWS_M] NV settings loaded: mode=%s vol=%u bri=%u bass=%u tone=%s len=%u\r\n",
                    mode_name(g_settings.mode), (unsigned)g_settings.volume, (unsigned)g_settings.brightness,
                    (unsigned)g_settings.bass, spi_tone_name(g_settings.tone), (unsigned)len);
        return 1;
    }

    osal_printk("[DWS_M] NV settings unavailable: ret=%d len=%u\r\n", (int)ret, (unsigned)len);
    return 0;
}

void spi_settings_nv_flush_if_idle(void)
{
    if (!g_nv_dirty) {
        return;
    }

    uint64_t now = uapi_systick_get_ms();
    if (now - g_nv_last_change_tick < NV_FLUSH_DELAY_MS) {
        return;
    }

    spi_settings_t saved = g_settings;
    saved.cmd = SPI_CMD_QUERY;
    saved.flags = 0;
    errcode_t ret = uapi_nv_write(NV_KEY_SPI_SETTINGS, (const uint8_t *)&saved, sizeof(saved));
    if (ret != ERRCODE_SUCC) {
        osal_printk("[DWS_M] NV write failed: %d\r\n", (int)ret);
    }
    g_nv_dirty = false;
}

void spi_settings_update_hotspot_network(uint8_t hotspot, uint8_t network)
{
    uint8_t packed = spi_make_hotspot_network(hotspot, network);
    if (spi_validate_hotspot_network(packed)) {
        g_settings.hotspot_network = packed;
        spi_settings_mark_sync(true);
    }
}

void spi_settings_update_mode(uint8_t mode)
{
    if (spi_validate_mode(mode)) {
        g_settings.mode = mode;
        spi_settings_mark_sync(true);
    }
}

void spi_settings_update_volume(uint8_t volume)
{
    if (spi_validate_percent(volume)) {
        g_settings.volume = volume;
        spi_settings_mark_sync(true);
    }
}

void spi_settings_update_brightness(uint8_t brightness)
{
    if (spi_validate_percent(brightness)) {
        g_settings.brightness = brightness;
        spi_settings_mark_sync(true);
    }
}

void spi_settings_update_bass(uint8_t bass)
{
    if (spi_validate_percent(bass)) {
        g_settings.bass = bass;
        spi_settings_mark_sync(true);
    }
}

void spi_settings_update_tone(uint8_t tone)
{
    if (spi_validate_tone(tone)) {
        g_settings.tone = tone;
        spi_settings_mark_sync(true);
    }
}

void spi_settings_update_night(uint8_t enabled)
{
    uint8_t flags = enabled ? (uint8_t)(g_settings.flags | SPI_FLAG_NIGHT)
                            : (uint8_t)(g_settings.flags & (uint8_t)~SPI_FLAG_NIGHT);
    if (flags != g_settings.flags) {
        g_settings.flags = flags;
        spi_settings_mark_sync(false);
    }
}

const char *spi_tone_name(uint8_t tone)
{
    switch (tone) {
        case SPI_TONE_FLAT:
            return "FLAT";
        case SPI_TONE_VOCAL:
            return "VOCAL";
        case SPI_TONE_BASS_BOOST:
            return "BASS_BOOST";
        case SPI_TONE_POP:
            return "POP";
        case SPI_TONE_ROCK:
            return "ROCK";
        default:
            return "UNKNOWN";
    }
}

uint8_t spi_settings_effective_volume(const spi_settings_t *s)
{
    if (s == nullptr) {
        return 0;
    }
    uint8_t volume = s->volume;
    if (spi_settings_is_night(s) && volume > 30) {
        volume = 30;
    }
    return volume;
}

uint8_t spi_settings_effective_bass(const spi_settings_t *s)
{
    if (s == nullptr) {
        return 0;
    }

    int bass = s->bass;
    switch (s->tone) {
        case SPI_TONE_VOCAL:
            bass -= 15;
            break;
        case SPI_TONE_BASS_BOOST:
            bass += 35;
            break;
        case SPI_TONE_POP:
            bass += 15;
            break;
        case SPI_TONE_ROCK:
            bass += 25;
            break;
        case SPI_TONE_FLAT:
        default:
            break;
    }

    if (bass < 0) {
        bass = 0;
    } else if (bass > 100) {
        bass = 100;
    }

    if (spi_settings_is_night(s) && bass > 10) {
        bass = 10;
    }
    return (uint8_t)bass;
}

void *spi_master_task(void *arg)
{
    (void)arg;
    static sed_ws63::dws_master spi;

    uint8_t rx_buf[sed_ws63::dws_master::TRANSFER_LEN];
    uint8_t prev_mode = g_settings.mode;
    uint8_t prev_volume = g_settings.volume;
    uint8_t prev_bass = g_settings.bass;
    uint8_t prev_tone = g_settings.tone;
    uint8_t prev_flags = g_settings.flags;

    while (true) {
        audio_analyzer::compute();
        const audio_result_t &audio = audio_analyzer::get_result();


        uint8_t tx_buf[sed_ws63::dws_master::TRANSFER_LEN] = {0};
        const uint8_t *settings_bytes = (const uint8_t *)&g_settings;
        for (int i = 0; i < SPI_SETTINGS_LEN; i++) {
            tx_buf[i] = settings_bytes[i];
        }
        /* 记录本帧实际发送的设置, 防止 transfer 期间被外部改掉后误清除 SYNC */
        spi_settings_t tx_settings_snapshot = g_settings;
        tx_buf[SPI_AUDIO_OFFSET + 0] = audio.bands[0];
        tx_buf[SPI_AUDIO_OFFSET + 2] = audio.bands[2];
        tx_buf[SPI_AUDIO_OFFSET + 3] = audio.bands[3];
        tx_buf[SPI_AUDIO_OFFSET + 5] = audio.overall;
        /* beat 仅 0/1, 校验后发出以防对端收到错位数据 */
        tx_buf[SPI_AUDIO_OFFSET + 6] = (audio.beat != 0) ? (uint8_t)1 : (uint8_t)0;

        int ret = spi.transfer(tx_buf, sed_ws63::dws_master::TRANSFER_LEN, rx_buf, sed_ws63::dws_master::TRANSFER_LEN);

        if (ret == 0) {
            spi_settings_t resp;
            for (int i = 0; i < SPI_SETTINGS_LEN; i++) {
                ((uint8_t *)&resp)[i] = rx_buf[i];
            }

            /* 首次通信从控制端同步一次; 运行中控制端本地变更通过回包 SYNC 推送。 */
            static bool boot_sync_done = false;
            if (!boot_sync_done && tx_settings_snapshot.cmd != SPI_CMD_SYNC && spi_validate_settings(&resp)) {
                spi_settings_apply_control_payload(&resp);
                g_settings.cmd = SPI_CMD_QUERY;
                boot_sync_done = true;
            }

            if (tx_settings_snapshot.cmd != SPI_CMD_SYNC && boot_sync_done && resp.cmd == SPI_CMD_SYNC &&
                spi_validate_settings(&resp)) {
                spi_settings_apply_control_payload(&resp);
                g_settings.cmd = SPI_CMD_SYNC;
            }

            if (tx_settings_snapshot.cmd == SPI_CMD_SYNC && spi_settings_payload_equal(&resp, &tx_settings_snapshot) &&
                spi_settings_payload_equal(&g_settings, &tx_settings_snapshot)) {
                g_settings.cmd = SPI_CMD_QUERY;
                boot_sync_done = true;
            }
        }

        if (g_settings.mode != prev_mode) {
            osal_printk("[DWS_M] MODE CHANGED: %s -> %s\r\n", mode_name(prev_mode), mode_name(g_settings.mode));
            prev_mode = g_settings.mode;
        }
        if (g_settings.volume != prev_volume) {
            prev_volume = g_settings.volume;
        }
        if (g_settings.bass != prev_bass) {
            prev_bass = g_settings.bass;
        }
        if (g_settings.tone != prev_tone) {
            prev_tone = g_settings.tone;
        }
        if (g_settings.flags != prev_flags) {
            prev_flags = g_settings.flags;
        }

        spi_settings_nv_flush_if_idle();
        osal_msleep(50);
    }
    return nullptr;
}
