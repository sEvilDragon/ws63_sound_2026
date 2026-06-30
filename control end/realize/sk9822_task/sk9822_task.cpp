#include "sk9822_task.h"
#include "sk9822_led.hpp"
#include "spi_task.h"

static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    h %= 360;
    uint8_t region = h / 60;
    uint16_t remainder = (h - region * 60) * 255 / 60;

    uint8_t p = (uint8_t)((uint16_t)v * (255 - s) / 255);
    uint8_t q = (uint8_t)((uint16_t)v * (255 - (uint16_t)s * remainder / 255) / 255);
    uint8_t t = (uint8_t)((uint16_t)v * (255 - (uint16_t)s * (255 - remainder) / 255) / 255);

    switch (region) {
        case 0:
            *r = v;
            *g = t;
            *b = p;
            break;
        case 1:
            *r = q;
            *g = v;
            *b = p;
            break;
        case 2:
            *r = p;
            *g = v;
            *b = t;
            break;
        case 3:
            *r = p;
            *g = q;
            *b = v;
            break;
        case 4:
            *r = t;
            *g = p;
            *b = v;
            break;
        default:
            *r = v;
            *g = p;
            *b = q;
            break;
    }
}

void *sk9822_task(void *arg)
{
    unused(arg);

    osal_printk("[SK9822] init OK, %d LEDs\r\n", sed_ws63::sk9822_led::NUM_LEDS);
    sed_ws63::sk9822_led led;

    uint16_t tick = 0;
    float smoothed_ov = 0.0f;  /* 平滑能量 0~255 */
    uint8_t beat_boost = 0;    /* 节拍脉冲计数 */

    while (true) {
        const audio_result_t *audio = get_audio_result();
        tick++;

        /* ===== 1. 快速平滑能量 (alpha=0.25) ===== */
        smoothed_ov += ((float)audio->overall - smoothed_ov) * 0.25f;

        /* ===== 2. 空闲呼吸 (三角波, ~3.5 秒周期) ===== */
        uint16_t breath_tick = tick % 116; /* 116*30ms ≈ 3.5s */
        float breath;
        if (breath_tick < 58) {
            breath = (float)breath_tick / 58.0f;       /* 0 → 1 */
        } else {
            breath = (float)(116 - breath_tick) / 58.0f; /* 1 → 0 */
        }
        breath = 0.25f + 0.75f * breath; /* 0.25 ~ 1.0 */

        /* ===== 3. 音乐强度: 0=空闲, 1=满幅 ===== */
        float music = smoothed_ov / 255.0f;
        float intensity = breath * (0.35f + 0.65f * music);
        /* 空闲时 intensity≈0.09~0.35, 满音乐时≈0.35~1.0 */

        /* ===== 4. 节拍脉冲 ===== */
        if (audio->beat && beat_boost == 0) {
            beat_boost = 6;
        }
        if (beat_boost > 0) {
            intensity *= 1.0f + (float)beat_boost * 0.12f; /* 最大 +72% */
            beat_boost--;
        }

        /* ===== 5. 高斯宽度: spread 越大 = 亮灯越多 ===== */
        float spread = 0.6f + intensity * 4.8f; /* 0.6~5.4, 覆盖 0~4 距离 */

        /* ===== 6. 色相: 极慢旋转 (~2.8°/s, 128s 一圈) ===== */
        uint16_t base_hue = (tick / 10) % 360;

        for (uint8_t i = 0; i < sed_ws63::sk9822_led::NUM_LEDS; i++) {
            /* 两排灯: 0~8 第一排, 9~17 第二排, 每排中心在位置 4 */
            uint8_t pos = i % 9;
            uint8_t dist_u = (pos > 4) ? (uint8_t)(pos - 4) : (uint8_t)(4 - pos);
            float dist = (float)dist_u; /* 0.0 ~ 4.0 */

            /* ===== 亮度: 类高斯衰减 (二次曲线), 多灯同时变化 ===== */
            float ratio = dist / spread;
            float fb;
            if (ratio >= 1.0f) {
                fb = 0.0f;
            } else {
                /* 平滑钟形: (1 - r²)(1 - 0.3r) */
                fb = (1.0f - ratio * ratio) * (1.0f - 0.3f * ratio);
            }
            uint8_t brightness = (uint8_t)(fb * 255.0f);

            /* 最小微光: 空闲时中心也有可见呼吸 */
            if (brightness < 4) {
                brightness = 4;
            }

            /* ===== 色相: 中心暖金(30°) → 边缘冷蓝(270°) ===== */
            uint16_t hue = (base_hue + (uint16_t)(dist * 28.0f)) % 360;

            /* ===== 饱和度: 偏鲜艳但柔和 ===== */
            uint8_t sat = 210;

            uint8_t r, g, b;
            hsv_to_rgb(hue, sat, brightness, &r, &g, &b);
            led.set_pixel(i, r, g, b, 15);
        }

        led.update();
        osal_msleep(30);
    }
    return NULL;
}
