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

#ifndef VOICE_BASS_STEP
#define VOICE_BASS_STEP 5
#endif

// 语音模块通过 0x64 寄存器输出完整 IIC 指令 AA 55 00 XX FB 中的 XX 字节。
#define VOICE_ID_NONE             0x00
#define VOICE_ID_MODE_SLE         0x01  // 切换星闪模式
#define VOICE_ID_MODE_DLNA        0x02  // 切换网络模式
#define VOICE_ID_NETWORK_ON       0x03  // 打开网络连接
#define VOICE_ID_HOTSPOT_ON       0x04  // 打开热点连接
#define VOICE_ID_NETWORK_OFF      0x05  // 关闭网络连接
#define VOICE_ID_HOTSPOT_OFF      0x06  // 关闭热点连接
#define VOICE_ID_BRIGHTNESS_UP    0x07  // 增大亮度
#define VOICE_ID_BRIGHTNESS_DOWN  0x08  // 减小亮度
#define VOICE_ID_BRIGHTNESS_MAX   0x09  // 亮度最大
#define VOICE_ID_VOLUME_UP         0x0A  // 增大音响音量
#define VOICE_ID_VOLUME_DOWN       0x0B  // 减小音响音量
#define VOICE_ID_VOLUME_MAX        0x0C  // 音响音量最大
#define VOICE_ID_VOLUME_MIN        0x0D  // 音响音量最小
#define VOICE_ID_NIGHT_ON          0x0E  // 开启夜间模式
#define VOICE_ID_NIGHT_OFF         0x0F  // 关闭夜间模式
#define VOICE_ID_BASS_UP           0x10  // 增大低音
#define VOICE_ID_BASS_DOWN         0x11  // 减小低音
#define VOICE_ID_BASS_MAX          0x12  // 低音最大
#define VOICE_ID_BASS_MIN          0x13  // 低音最小
#define VOICE_ID_TONE_FLAT         0x14  // 默认音效
#define VOICE_ID_TONE_VOCAL        0x15  // 人声音效
#define VOICE_ID_TONE_BASS_BOOST   0x16  // 低音增强音效
#define VOICE_ID_TONE_POP          0x17  // 流行音效
#define VOICE_ID_TONE_ROCK         0x18  // 摇滚音效
#define VOICE_ID_INTRODUCE_SELF    0x19  // 介绍自己（模块自行播报）
#define VOICE_ID_BRIGHTNESS_MIN    0x66  // 亮度最小

#define VOICE_PROMPT_TYPE          0xFF
#define VOICE_PROMPT_QUEUE_LEN     8

namespace {

static uint8_t g_prompt_queue[VOICE_PROMPT_QUEUE_LEN] = {0};
static uint8_t g_prompt_queue_read = 0;
static uint8_t g_prompt_queue_write = 0;
static uint8_t g_prompt_queue_count = 0;

static bool is_valid_prompt(uint8_t prompt)
{
    switch (prompt) {
        case VOICE_PROMPT_MODE_SELECTION:
        case VOICE_PROMPT_VOLUME_ADJUST:
        case VOICE_PROMPT_BRIGHTNESS_ADJUST:
        case VOICE_PROMPT_NETWORK_ON:
        case VOICE_PROMPT_NETWORK_OFF:
        case VOICE_PROMPT_HOTSPOT_ON:
        case VOICE_PROMPT_HOTSPOT_OFF:
            return true;
        default:
            return false;
    }
}

static bool enqueue_prompt(uint8_t prompt)
{
    unsigned long irq = osal_irq_lock();
    if (g_prompt_queue_count >= VOICE_PROMPT_QUEUE_LEN) {
        osal_irq_restore(irq);
        return false;
    }
    g_prompt_queue[g_prompt_queue_write] = prompt;
    g_prompt_queue_write = (uint8_t)((g_prompt_queue_write + 1) % VOICE_PROMPT_QUEUE_LEN);
    g_prompt_queue_count++;
    osal_irq_restore(irq);
    return true;
}

static bool dequeue_prompt(uint8_t *prompt)
{
    if (prompt == nullptr) {
        return false;
    }

    unsigned long irq = osal_irq_lock();
    if (g_prompt_queue_count == 0) {
        osal_irq_restore(irq);
        return false;
    }
    *prompt = g_prompt_queue[g_prompt_queue_read];
    g_prompt_queue_read = (uint8_t)((g_prompt_queue_read + 1) % VOICE_PROMPT_QUEUE_LEN);
    g_prompt_queue_count--;
    osal_irq_restore(irq);
    return true;
}

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

static void update_bass_delta(int delta)
{
    const spi_settings_t *s = get_spi_settings();
    spi_settings_update_bass(clamp_percent_int((int)s->bass + delta));
}

static void update_hotspot(uint8_t hotspot)
{
    const spi_settings_t *s = get_spi_settings();
    spi_settings_update_hotspot_network(hotspot, spi_get_network(s->hotspot_network));
}

static void update_network(uint8_t network)
{
    const spi_settings_t *s = get_spi_settings();
    // 打开 STA 时主动退出 SoftAP，避免产生 hotspot/network 同时开启的状态。
    uint8_t hotspot = spi_get_hotspot(s->hotspot_network);
    if (network == SPI_NETWORK_CONN) {
        hotspot = SPI_HOTSPOT_OFF;
    }
    spi_settings_update_hotspot_network(hotspot, network);
}

static bool read_voice_id(sed_ws63::iic_master &iic, uint8_t *id)
{
    if (id == nullptr) {
        return false;
    }

    uint8_t reg = VOICE_RESULT_REG;
    return iic.iic_master_read(&reg, 1, id, 1, VOICE_I2C_ADDR);
}

static bool send_passive_prompt(sed_ws63::iic_master &iic, uint8_t prompt)
{
    // PDF 4.2：向 0x6E 写入“类型 + ID”；对应逻辑帧 AA 55 FF ID FB。
    uint8_t tx[3] = {VOICE_SPEAK_REG, VOICE_PROMPT_TYPE, prompt};
    return iic.iic_master_write(tx, sizeof(tx), VOICE_I2C_ADDR);
}

static bool apply_voice_id(uint8_t id)
{
    switch (id) {
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
        case VOICE_ID_BRIGHTNESS_MAX: // 亮度最大
            spi_settings_update_brightness(100);
            return true;
        case VOICE_ID_VOLUME_UP: // 增大音响音量
            update_volume_delta(VOICE_VOLUME_STEP);
            return true;
        case VOICE_ID_VOLUME_DOWN: // 减小音响音量
            update_volume_delta(-VOICE_VOLUME_STEP);
            return true;
        case VOICE_ID_VOLUME_MAX: // 音响音量最大
            spi_settings_update_volume(100);
            return true;
        case VOICE_ID_VOLUME_MIN: // 音响音量最小
            spi_settings_update_volume(0);
            return true;
        case VOICE_ID_NIGHT_ON: // 开启夜间模式
            spi_settings_update_night(1);
            return true;
        case VOICE_ID_NIGHT_OFF: // 关闭夜间模式
            spi_settings_update_night(0);
            return true;
        case VOICE_ID_BASS_UP: // 增大低音
            update_bass_delta(VOICE_BASS_STEP);
            return true;
        case VOICE_ID_BASS_DOWN: // 减小低音
            update_bass_delta(-VOICE_BASS_STEP);
            return true;
        case VOICE_ID_BASS_MAX: // 低音最大
            spi_settings_update_bass(100);
            return true;
        case VOICE_ID_BASS_MIN: // 低音最小
            spi_settings_update_bass(0);
            return true;
        case VOICE_ID_TONE_FLAT: // 默认音效
            spi_settings_update_tone(SPI_TONE_FLAT);
            return true;
        case VOICE_ID_TONE_VOCAL: // 人声音效
            spi_settings_update_tone(SPI_TONE_VOCAL);
            return true;
        case VOICE_ID_TONE_BASS_BOOST: // 低音增强音效
            spi_settings_update_tone(SPI_TONE_BASS_BOOST);
            return true;
        case VOICE_ID_TONE_POP: // 流行音效
            spi_settings_update_tone(SPI_TONE_POP);
            return true;
        case VOICE_ID_TONE_ROCK: // 摇滚音效
            spi_settings_update_tone(SPI_TONE_ROCK);
            return true;
        case VOICE_ID_INTRODUCE_SELF:
            // 该指令的回复由语音模块自身完成，控制端不发送反向播报命令。
            return true;
        case VOICE_ID_BRIGHTNESS_MIN: // 亮度最小（最新表中独立使用 0x66）
            spi_settings_update_brightness(0);
            return true;
        default:
            return false;
    }
}

} // namespace

bool voice_request_prompt(voice_prompt_t prompt)
{
    uint8_t id = (uint8_t)prompt;
    if (!is_valid_prompt(id)) {
        osal_printk("[VOICE] reject invalid passive prompt id=0x%02X\r\n", id);
        return false;
    }
    if (!enqueue_prompt(id)) {
        osal_printk("[VOICE] passive prompt queue full, id=0x%02X\r\n", id);
        return false;
    }
    return true;
}

bool voice_request_mode_selection_prompt(void)
{
    return voice_request_prompt(VOICE_PROMPT_MODE_SELECTION);
}

bool voice_request_volume_adjust_prompt(void)
{
    return voice_request_prompt(VOICE_PROMPT_VOLUME_ADJUST);
}

bool voice_request_brightness_adjust_prompt(void)
{
    return voice_request_prompt(VOICE_PROMPT_BRIGHTNESS_ADJUST);
}

bool voice_request_network_on_prompt(void)
{
    return voice_request_prompt(VOICE_PROMPT_NETWORK_ON);
}

bool voice_request_network_off_prompt(void)
{
    return voice_request_prompt(VOICE_PROMPT_NETWORK_OFF);
}

bool voice_request_hotspot_on_prompt(void)
{
    return voice_request_prompt(VOICE_PROMPT_HOTSPOT_ON);
}

bool voice_request_hotspot_off_prompt(void)
{
    return voice_request_prompt(VOICE_PROMPT_HOTSPOT_OFF);
}

void *voice_task(void *arg)
{
    (void)arg;

    sed_ws63::iic_master iic(GPIO_15, GPIO_16);
    uint8_t id = VOICE_ID_NONE;
    uint8_t last_id = VOICE_ID_NONE;
    uint8_t prompt = 0;
    uint64_t last_accept_ms = 0;
    int fail_count = 0;

    osal_printk("[VOICE] task started, i2c addr=0x%02X result_reg=0x%02X\r\n",
                (unsigned)VOICE_I2C_ADDR, (unsigned)VOICE_RESULT_REG);

    while (true) {
        if (dequeue_prompt(&prompt)) {
            bool sent = send_passive_prompt(iic, prompt);
            osal_printk("[VOICE] passive prompt %s: AA 55 FF %02X FB\r\n", sent ? "sent" : "failed", prompt);
        }

        if (!read_voice_id(iic, &id)) {
            fail_count++;
            if ((fail_count % 200) == 1) {
                osal_printk("[VOICE] i2c read fail #%d\r\n", fail_count);
            }
            osal_msleep(VOICE_I2C_POLL_MS);
            continue;
        }

        if (fail_count > 0) {
            osal_printk("[VOICE] i2c recovered after %d failures\r\n", fail_count);
        }
        fail_count = 0;

        if (id == VOICE_ID_NONE) {
            // 每 5 秒 (~100 次轮询) 输出一次心跳, 确认 I2C 链路正常
            static int heartbeat = 0;
            if (++heartbeat >= 100) {
                heartbeat = 0;
                osal_printk("[VOICE] alive, waiting for command...\r\n");
            }
            osal_msleep(VOICE_I2C_POLL_MS);
            continue;
        }

        uint64_t now = uapi_systick_get_ms();
        if (id == last_id && (now - last_accept_ms) < VOICE_REPEAT_SUPPRESS_MS) {
            osal_msleep(VOICE_I2C_POLL_MS);
            continue;
        }

        bool handled = apply_voice_id(id);
        if (handled) {
            const spi_settings_t *s = get_spi_settings();
            osal_printk("[VOICE] handled id=0x%02X mode=%u vol=%u bri=%u bass=%u tone=%u flags=0x%02X hs_nw=0x%02X\r\n",
                        id, (unsigned)s->mode, (unsigned)s->volume, (unsigned)s->brightness,
                        (unsigned)s->bass, (unsigned)s->tone, (unsigned)s->flags,
                        (unsigned)s->hotspot_network);
        } else {
            osal_printk("[VOICE] ignored id=0x%02X\r\n", id);
        }

        if (handled) {
            last_id = id;
            last_accept_ms = now;
        }

        osal_msleep(VOICE_I2C_POLL_MS);
    }

    return nullptr;
}
