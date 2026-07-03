#include "sk9822_task.h"
#include "sk9822_led.hpp"
#include "spi_task.h"
#include <math.h>

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
    float smoothed_ov = 0.0f; /* 平滑能量 0~255 */
    float fx_level = 0.0f;    /* 音乐灯效强度, 慢释放避免效果一闪就没 */
    uint8_t beat_boost = 0;   /* 节拍脉冲计数 */
    float base_hue = 0.0f;   /* 增量累加色相, 避免速度变化时跳变 */
    uint16_t prev_hue0 = 0;  /* 诊断: 上一帧 LED0 色相 */

    while (true) {
        const audio_result_t *audio = get_audio_result();
        tick++;

        /* ===== 1. 平滑能量 (alpha=0.3, 限制单帧跳变) ===== */
        float diff = (float)audio->overall - smoothed_ov;
        if (diff > 60.0f)
            diff = 60.0f; /* 防止 SPI 毛刺引起跳变 */
        if (diff < -60.0f)
            diff = -60.0f;
        smoothed_ov += diff * 0.3f;
        float music = smoothed_ov / 255.0f; /* 0=空闲, 1=满幅 */

        /* 静音时只保留原来的环形 RGB 流动; 音乐层做慢释放, 避免一闪就没 */
        float music_fx = smoothed_ov / 255.0f;
        if (audio->beat && music_fx < 0.35f)
            music_fx = 0.35f;
        if (music_fx > fx_level) {
            fx_level += (music_fx - fx_level) * 0.55f; /* 快速响应 */
        } else {
            fx_level *= 0.96f; /* 慢释放: 暂停后约 1s 多逐渐淡出 */
        }
        if (fx_level < 0.015f)
            fx_level = 0.0f;
        if (fx_level > 1.0f)
            fx_level = 1.0f;

        /* ===== 2. 节拍脉冲: 只在有明显音乐时触发 ===== */
        if (fx_level > 0.02f && audio->beat && beat_boost == 0) {
            beat_boost = 6;
        }
        float beat_factor = 1.0f;
        if (beat_boost > 0) {
            beat_factor = 1.0f + (float)beat_boost * 0.08f; /* 最多 +48%, 节拍更清楚 */
            beat_boost--;
        }

        /* ===== 3. 空闲为环形彩虹; 有音乐时叠加中心对称动态层 ===== */
        float flow_speed = 3.0f + fx_level * 10.0f; /* 降低基础转圈速度, 音乐强时再加速 */
        base_hue += flow_speed;
        if (base_hue >= 360.0f)
            base_hue -= 360.0f;
        uint16_t hue_int = (uint16_t)base_hue;

        const spi_settings_t *settings = get_spi_settings();
        uint8_t led_brightness = settings->brightness == 0 ? 0 : (uint8_t)(3 + settings->brightness * 28 / 100);

        /* 音乐层: 弹簧式伸缩 + 左右相位摇晃; 静音时 fx_level=0, 不参与显示 */
        float spring = sinf((float)tick * 0.55f) * fx_level;        /* 伸缩相位 */
        float spring_scale = 1.0f + spring * 0.48f * beat_factor;  /* 音乐越强, 伸缩幅度越大 */
        float phase_wobble = sinf((float)tick * 0.23f) * fx_level * 0.82f;
        float hue_wobble = sinf((float)tick * 0.19f + fx_level * 3.0f) * 30.0f * fx_level;

        for (uint8_t i = 0; i < sed_ws63::sk9822_led::NUM_LEDS; i++) {
            /* 基础层按物理链路 0..17 环形流动: 第一排 1..9, 第二排 18..10 正好绕一圈 */
            uint16_t hue = (hue_int + (uint16_t)i * 20) % 360;
            uint8_t sat_base = 235;
            uint8_t bright_base = (uint8_t)(34.0f + fx_level * 12.0f); /* 底色压低, 避免盖住音乐层 */
            uint8_t base_r, base_g, base_b;
            hsv_to_rgb(hue, sat_base, bright_base, &base_r, &base_g, &base_b);

            /* 物理排列: 第一排 1..9, 第二排 18..10, 第二排反向映射得到视觉列 */
            uint8_t pos = (i < 9) ? i : (uint8_t)(17 - i); /* 视觉位置 0..8, 左到右 */
            bool upper_row = (i < 9);

            /* 两排略错相: 仍围绕中心, 但不完全镜像 */
            float row_phase = upper_row ? phase_wobble : -phase_wobble;
            float row_hue_shift = upper_row ? 0.0f : 24.0f;
            float visual_pos = (float)pos + row_phase;

            /* ===== 音乐层: 把 0..8 的视觉坐标围绕中心做伸缩, 形成弹簧/音符跳动感 ===== */
            float spring_pos = 4.0f + (visual_pos - 4.0f) * spring_scale;
            float spring_wave = cosf((spring_pos - 4.0f) * 2.15f + (float)tick * 0.42f + (upper_row ? 0.35f : -0.35f));
            float spring_env = spring_wave * 0.5f + 0.5f;

            /* 中心附近更亮, 两侧随伸缩拉开; 幅度由音乐强度控制 */
            float center_weight = 1.0f - fabsf(spring_pos - 4.0f) / 5.4f;
            if (center_weight < 0.0f)
                center_weight = 0.0f;
            float overlay_env = 0.35f + spring_env * 0.65f;
            overlay_env *= 0.45f + center_weight * 0.75f;

            float overlay_k = fx_level * (170.0f + fx_level * 520.0f) * overlay_env * beat_factor;

            /* 扫光颜色: 与底色错开, 随伸缩相位和上下排偏移 */
            int sweep_hue_i = (int)((float)hue_int + 125.0f + fx_level * 170.0f +
                                    hue_wobble + row_hue_shift + spring * 45.0f);
            while (sweep_hue_i < 0)
                sweep_hue_i += 360;
            sweep_hue_i %= 360;
            uint8_t sh_r, sh_g, sh_b;
            hsv_to_rgb((uint16_t)sweep_hue_i, 255, 255, &sh_r, &sh_g, &sh_b);

            /* 颜色叠加: 先把底色向音乐色相混合, 再少量加亮; 避免只看到亮度变化 */
            float color_mix = fx_level * overlay_env * beat_factor;
            if (color_mix > 0.88f)
                color_mix = 0.88f;
            float lift_k = overlay_k * 0.28f;

            float fr = (float)base_r * (1.0f - color_mix) + (float)sh_r * color_mix + lift_k * (float)sh_r / 255.0f;
            float fg = (float)base_g * (1.0f - color_mix) + (float)sh_g * color_mix + lift_k * (float)sh_g / 255.0f;
            float fb = (float)base_b * (1.0f - color_mix) + (float)sh_b * color_mix + lift_k * (float)sh_b / 255.0f;

            if (fr > 255.0f) fr = 255.0f;
            if (fg > 255.0f) fg = 255.0f;
            if (fb > 255.0f) fb = 255.0f;

            uint8_t r = (uint8_t)fr;
            uint8_t g = (uint8_t)fg;
            uint8_t b = (uint8_t)fb;

            led.set_pixel(i, r, g, b, led_brightness);
        }

        led.update();

        /* 诊断: 检测 LED0 色相跳变 (正常增量 3~15°, 超过 20° 即异常) */
        {
            int16_t dh = (int16_t)hue_int - (int16_t)prev_hue0;
            if (dh < -180) dh += 360;
            if (dh > 180) dh -= 360;
            if (dh > 20 || dh < -20) {
                osal_printk("[JUMP] hue %d->%d (d=%d) ov=%u music=%.2f spd=%.1f\r\n",
                            prev_hue0, hue_int, dh, audio->overall, (double)music, (double)flow_speed);
            }
            prev_hue0 = hue_int;
        }

        osal_msleep(30);
    }
    return NULL;
}
