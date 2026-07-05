#include "voice_task.h"
#include "spi_task.h"
#include "iic.hpp"

extern "C" {
#include "gpio.h"
#include "soc_osal.h"
#include "systick.h"
}

#ifndef VOICE_I2C_ADDR
#define VOICE_I2C_ADDR 0x64
#endif

#ifndef VOICE_I2C_POLL_MS
#define VOICE_I2C_POLL_MS 50
#endif

#ifndef VOICE_I2C_READ_REG
#define VOICE_I2C_READ_REG 0xFF
#endif

#ifndef VOICE_REPEAT_SUPPRESS_MS
#define VOICE_REPEAT_SUPPRESS_MS 700
#endif

#ifndef VOICE_VOLUME_STEP
#define VOICE_VOLUME_STEP 5
#endif

#ifndef VOICE_BRIGHTNESS_STEP
#define VOICE_BRIGHTNESS_STEP 10
#endif

#define VOICE_FRAME_LEN 5
#define VOICE_FRAME_HEAD0 0xAA
#define VOICE_FRAME_HEAD1 0x55
#define VOICE_FRAME_TAIL  0xFB

namespace {

static uint8_t clamp_percent_int(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 100) {
        return 100;
    }
    return (uint8_t)v;
}

static bool is_voice_frame(const uint8_t frame[VOICE_FRAME_LEN])
{
    return frame[0] == VOICE_FRAME_HEAD0 && frame[1] == VOICE_FRAME_HEAD1 && frame[4] == VOICE_FRAME_TAIL;
}

static bool same_frame(const uint8_t a[VOICE_FRAME_LEN], const uint8_t b[VOICE_FRAME_LEN])
{
    for (int i = 0; i < VOICE_FRAME_LEN; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static void copy_frame(uint8_t dst[VOICE_FRAME_LEN], const uint8_t src[VOICE_FRAME_LEN])
{
    for (int i = 0; i < VOICE_FRAME_LEN; ++i) {
        dst[i] = src[i];
    }
}

static void update_volume_delta(int delta)
{
    const spi_settings_t *s = get_spi_settings();
    spi_settings_update_volume(clamp_percent_int((int)s->volume + delta));
}

static void update_brightness_delta(int delta)
{
    const spi_settings_t *s = get_spi_settings();
    spi_settings_update_brightness(clamp_percent_int((int)s->brightness + delta));
}

static void update_hotspot(uint8_t hotspot)
{
    const spi_settings_t *s = get_spi_settings();
    spi_settings_update_hotspot_network(hotspot, spi_get_network(s->hotspot_network));
}

static void update_network(uint8_t network)
{
    const spi_settings_t *s = get_spi_settings();
    spi_settings_update_hotspot_network(spi_get_hotspot(s->hotspot_network), network);
}

static bool apply_voice_frame(const uint8_t frame[VOICE_FRAME_LEN])
{
    const uint8_t group = frame[2];
    const uint8_t cmd = frame[3];

    if (cmd == 0x00) {
        switch (group) {
            case 0x03: // 小闪小闪：唤醒词，语音模块自己播报，音响端无配置变化。
                return true;
            case 0x04: // 增大音量
                update_volume_delta(VOICE_VOLUME_STEP);
                return true;
            case 0x05: // 减小音量
                update_volume_delta(-VOICE_VOLUME_STEP);
                return true;
            case 0x06: // 最大音量
                spi_settings_update_volume(100);
                return true;
            case 0x07: // 中等音量
                spi_settings_update_volume(50);
                return true;
            case 0x08: // 最小音量
                spi_settings_update_volume(0);
                return true;
            default:
                return false;
        }
    }

    if (group != 0x00) {
        return false;
    }

    switch (cmd) {
        case 0x01: // 切换星闪模式
            spi_settings_update_mode(SPI_MODE_SLE);
            return true;
        case 0x02: // 切换网络模式
            spi_settings_update_mode(SPI_MODE_DLNA);
            return true;
        case 0x03: // 打开网络连接
            update_network(SPI_NETWORK_CONN);
            return true;
        case 0x04: // 打开热点连接
            update_hotspot(SPI_HOTSPOT_ON);
            return true;
        case 0x05: // 关闭网络连接
            update_network(SPI_NETWORK_DISC);
            return true;
        case 0x06: // 关闭热点连接
            update_hotspot(SPI_HOTSPOT_OFF);
            return true;
        case 0x07: // 增大亮度
            update_brightness_delta(VOICE_BRIGHTNESS_STEP);
            return true;
        case 0x08: // 减小亮度
            update_brightness_delta(-VOICE_BRIGHTNESS_STEP);
            return true;
        case 0x09: // Excel 中“亮度最大/亮度最小”共用协议；当前先按“亮度最大”处理。
            spi_settings_update_brightness(100);
            return true;
        case 0x1B: // 介绍自己：语音模块主动播报，音响端无配置变化。
            return true;
        default:
            return false;
    }
}

static bool read_voice_frame(sed_ws63::iic_master &iic, uint8_t frame[VOICE_FRAME_LEN])
{
#if VOICE_I2C_READ_REG == 0xFF
    return iic.iic_master_read_only(frame, VOICE_FRAME_LEN, VOICE_I2C_ADDR);
#else
    uint8_t reg = (uint8_t)VOICE_I2C_READ_REG;
    return iic.iic_master_read(&reg, 1, frame, VOICE_FRAME_LEN, VOICE_I2C_ADDR);
#endif
}

} // namespace

void *voice_task(void *arg)
{
    (void)arg;

    sed_ws63::iic_master iic(GPIO_15, GPIO_16);
    uint8_t frame[VOICE_FRAME_LEN] = {0};
    uint8_t last_frame[VOICE_FRAME_LEN] = {0};
    uint64_t last_accept_ms = 0;
    int fail_count = 0;

    osal_printk("[VOICE] task started, i2c addr=0x%02X\r\n", (unsigned)VOICE_I2C_ADDR);

    while (true) {
        if (!read_voice_frame(iic, frame)) {
            fail_count++;
            if ((fail_count % 200) == 1) {
                osal_printk("[VOICE] i2c read fail #%d\r\n", fail_count);
            }
            osal_msleep(VOICE_I2C_POLL_MS);
            continue;
        }
        fail_count = 0;

        if (!is_voice_frame(frame)) {
            osal_msleep(VOICE_I2C_POLL_MS);
            continue;
        }

        uint64_t now = uapi_systick_get_ms();
        if (same_frame(frame, last_frame) && (now - last_accept_ms) < VOICE_REPEAT_SUPPRESS_MS) {
            osal_msleep(VOICE_I2C_POLL_MS);
            continue;
        }

        bool handled = apply_voice_frame(frame);
        osal_printk("[VOICE] %s frame=%02X %02X %02X %02X %02X\r\n",
                    handled ? "handled" : "ignored",
                    frame[0], frame[1], frame[2], frame[3], frame[4]);

        if (handled) {
            copy_frame(last_frame, frame);
            last_accept_ms = now;
        }

        osal_msleep(VOICE_I2C_POLL_MS);
    }

    return nullptr;
}
