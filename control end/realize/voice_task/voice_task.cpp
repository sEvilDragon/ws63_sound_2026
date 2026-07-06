#include "voice_task.h"
#include "spi_task.h"
#include "iic.hpp"

extern "C" {
#include "gpio.h"
#include "soc_osal.h"
#include "systick.h"
}

/* 语音交互模块 IIC 从机地址与寄存器。 */
#ifndef VOICE_I2C_ADDR
#define VOICE_I2C_ADDR 0x34
#endif

#ifndef VOICE_RESULT_REG
#define VOICE_RESULT_REG 0x64
#endif

#ifndef VOICE_SPEAK_REG
#define VOICE_SPEAK_REG 0x6E
#endif

#ifndef VOICE_I2C_POLL_MS
#define VOICE_I2C_POLL_MS 50
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

/* 默认不主动写 0x6E 播报寄存器，避免与语音模块“主动播报”重复。
 * 若后续需要由 control 端确认处理完成后再播报，可在编译参数中置 1。 */
#ifndef VOICE_ECHO_AFTER_HANDLE
#define VOICE_ECHO_AFTER_HANDLE 0
#endif

#define VOICE_ID_NONE             0x00
#define VOICE_ID_WAKE             0x03
#define VOICE_ID_VOLUME_UP        0x04
#define VOICE_ID_VOLUME_DOWN      0x05
#define VOICE_ID_VOLUME_MAX       0x06
#define VOICE_ID_VOLUME_MID       0x07
#define VOICE_ID_VOLUME_MIN       0x08
#define VOICE_ID_MODE_SLE         0x0B
#define VOICE_ID_MODE_DLNA        0x0C
#define VOICE_ID_NETWORK_ON       0x0D
#define VOICE_ID_HOTSPOT_ON       0x0E
#define VOICE_ID_NETWORK_OFF      0x0F
#define VOICE_ID_HOTSPOT_OFF      0x10
#define VOICE_ID_BRIGHTNESS_UP    0x11
#define VOICE_ID_BRIGHTNESS_DOWN  0x12
#define VOICE_ID_BRIGHTNESS_LIMIT 0x13
#define VOICE_ID_INTRODUCE_SELF   0x25

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

static bool read_voice_id(sed_ws63::iic_master &iic, uint8_t *id)
{
    if (id == nullptr) {
        return false;
    }

    uint8_t reg = VOICE_RESULT_REG;
    return iic.iic_master_read(&reg, 1, id, 1, VOICE_I2C_ADDR);
}

#if VOICE_ECHO_AFTER_HANDLE
static void request_voice_speak(sed_ws63::iic_master &iic, uint8_t type, uint8_t id)
{
    /* 常见寄存器写法：先发寄存器地址 0x6E，再发 2 字节数据。 */
    uint8_t tx[3] = {VOICE_SPEAK_REG, type, id};
    (void)iic.iic_master_write(tx, sizeof(tx), VOICE_I2C_ADDR);
}
#endif

static bool apply_voice_id(uint8_t id)
{
    switch (id) {
        case VOICE_ID_WAKE: // 小闪小闪：唤醒词，语音模块自己播报，音响端无配置变化。
            return true;
        case VOICE_ID_VOLUME_UP: // 增大音量
            update_volume_delta(VOICE_VOLUME_STEP);
            return true;
        case VOICE_ID_VOLUME_DOWN: // 减小音量
            update_volume_delta(-VOICE_VOLUME_STEP);
            return true;
        case VOICE_ID_VOLUME_MAX: // 最大音量
            spi_settings_update_volume(100);
            return true;
        case VOICE_ID_VOLUME_MID: // 中等音量
            spi_settings_update_volume(50);
            return true;
        case VOICE_ID_VOLUME_MIN: // 最小音量
            spi_settings_update_volume(0);
            return true;
        case VOICE_ID_MODE_SLE: // 切换星闪模式
            spi_settings_update_mode(SPI_MODE_SLE);
            return true;
        case VOICE_ID_MODE_DLNA: // 切换网络模式
            spi_settings_update_mode(SPI_MODE_DLNA);
            return true;
        case VOICE_ID_NETWORK_ON: // 打开网络连接
            update_network(SPI_NETWORK_CONN);
            return true;
        case VOICE_ID_HOTSPOT_ON: // 打开热点连接
            update_hotspot(SPI_HOTSPOT_ON);
            return true;
        case VOICE_ID_NETWORK_OFF: // 关闭网络连接
            update_network(SPI_NETWORK_DISC);
            return true;
        case VOICE_ID_HOTSPOT_OFF: // 关闭热点连接
            update_hotspot(SPI_HOTSPOT_OFF);
            return true;
        case VOICE_ID_BRIGHTNESS_UP: // 增大亮度
            update_brightness_delta(VOICE_BRIGHTNESS_STEP);
            return true;
        case VOICE_ID_BRIGHTNESS_DOWN: // 减小亮度
            update_brightness_delta(-VOICE_BRIGHTNESS_STEP);
            return true;
        case VOICE_ID_BRIGHTNESS_LIMIT:
            /* Excel 中“亮度最大/亮度最小”语义标签相同，IIC 只返回 1 字节 ID，无法区分。
             * 当前先按“亮度最大”处理。 */
            spi_settings_update_brightness(100);
            return true;
        case VOICE_ID_INTRODUCE_SELF: // 介绍自己：语音模块主动播报，音响端无配置变化。
            return true;
        default:
            return false;
    }
}

} // namespace

void *voice_task(void *arg)
{
    (void)arg;

    sed_ws63::iic_master iic(GPIO_15, GPIO_16);
    uint8_t id = VOICE_ID_NONE;
    uint8_t last_id = VOICE_ID_NONE;
    uint64_t last_accept_ms = 0;
    int fail_count = 0;

    osal_printk("[VOICE] task started, i2c addr=0x%02X result_reg=0x%02X\r\n",
                (unsigned)VOICE_I2C_ADDR, (unsigned)VOICE_RESULT_REG);

    while (true) {
        if (!read_voice_id(iic, &id)) {
            fail_count++;
            if ((fail_count % 200) == 1) {
                osal_printk("[VOICE] i2c read fail #%d\r\n", fail_count);
            }
            osal_msleep(VOICE_I2C_POLL_MS);
            continue;
        }
        fail_count = 0;

        if (id == VOICE_ID_NONE) {
            osal_msleep(VOICE_I2C_POLL_MS);
            continue;
        }

        uint64_t now = uapi_systick_get_ms();
        if (id == last_id && (now - last_accept_ms) < VOICE_REPEAT_SUPPRESS_MS) {
            osal_msleep(VOICE_I2C_POLL_MS);
            continue;
        }

        bool handled = apply_voice_id(id);
        osal_printk("[VOICE] %s id=0x%02X\r\n", handled ? "handled" : "ignored", id);

        if (handled) {
#if VOICE_ECHO_AFTER_HANDLE
            request_voice_speak(iic, 0x00, id);
#endif
            last_id = id;
            last_accept_ms = now;
        }

        osal_msleep(VOICE_I2C_POLL_MS);
    }

    return nullptr;
}
