#include "sk9822_task.h"
#include "sk9822_led.hpp"
#include "spi_task.h"
#include <math.h>

static constexpr uint8_t VISUAL_COLS = 9;
static constexpr uint16_t FRAME_MS = 30;
static constexpr uint16_t VOLUME_OVERLAY_MS = 1000;
static constexpr uint16_t MODE_WAVE_MS = 720;
static constexpr uint16_t SLIDE_OVERLAY_MS = 600;

enum class overlay_type_t : uint8_t {
    NONE = 0,
    VOLUME,
    MODE_WAVE,
    SLIDE,
};

enum class slide_dir_t : uint8_t {
    LEFT_TO_RIGHT = 0,
    RIGHT_TO_LEFT,
};

struct overlay_state_t {
    overlay_type_t type;
    uint16_t age_ms;
    uint16_t duration_ms;
    uint8_t value;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    slide_dir_t dir;
};

static uint8_t overlay_priority(overlay_type_t type)
{
    switch (type) {
        case overlay_type_t::MODE_WAVE:
            return 3;
        case overlay_type_t::SLIDE:
            return 2;
        case overlay_type_t::VOLUME:
            return 1;
        default:
            return 0;
    }
}

static void start_overlay(overlay_state_t *overlay, const overlay_state_t &next)
{
    if (overlay_priority(next.type) >= overlay_priority(overlay->type)) {
        *overlay = next;
    }
}

static uint8_t visual_col_from_index(uint8_t i)
{
    return (i < VISUAL_COLS) ? i : (uint8_t)(17 - i);
}

static void set_visual_column(sed_ws63::sk9822_led &led, uint8_t col, uint8_t r, uint8_t g, uint8_t b,
                              uint8_t brightness)
{
    if (col >= VISUAL_COLS) {
        return;
    }
    led.set_pixel(col, r, g, b, brightness);
    led.set_pixel((uint8_t)(17 - col), r, g, b, brightness);
}

static uint8_t clamp_u8(float v)
{
    if (v <= 0.0f) {
        return 0;
    }
    if (v >= 255.0f) {
        return 255;
    }
    return (uint8_t)v;
}

static uint8_t white_level_from_brightness(uint8_t brightness_percent)
{
    if (brightness_percent == 0) {
        return 0;
    }
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    return (uint8_t)(24 + (uint16_t)brightness_percent * 156 / 100);
}

static void scale_overlay_color(uint8_t src_r, uint8_t src_g, uint8_t src_b, float strength,
                                uint8_t brightness_percent, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (strength <= 0.0f) {
        *r = 0;
        *g = 0;
        *b = 0;
        return;
    }
    if (strength > 1.0f) {
        strength = 1.0f;
    }

    float white_cap = 1.0f;
    if (src_r > 200 && src_g > 200 && src_b > 200) {
        white_cap = (float)white_level_from_brightness(brightness_percent) / 255.0f;
    }

    *r = clamp_u8((float)src_r * strength * white_cap);
    *g = clamp_u8((float)src_g * strength * white_cap);
    *b = clamp_u8((float)src_b * strength * white_cap);
}

static void mode_color(uint8_t mode, uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (mode) {
        case SPI_MODE_SLE:
        case SPI_MODE_SLE_MIC:
            *r = 0;
            *g = 255;
            *b = 0;
            break;
        case SPI_MODE_DLNA:
        case SPI_MODE_DLNA_NET:
            *r = 255;
            *g = 0;
            *b = 0;
            break;
        case SPI_MODE_WIREED:
        default:
            *r = 255;
            *g = 255;
            *b = 255;
            break;
    }
}

static void start_volume_overlay(overlay_state_t *overlay, uint8_t volume)
{
    overlay_state_t next = {overlay_type_t::VOLUME, 0, VOLUME_OVERLAY_MS, volume, 255, 255, 255,
                            slide_dir_t::LEFT_TO_RIGHT};
    start_overlay(overlay, next);
}

static void start_mode_overlay(overlay_state_t *overlay, uint8_t mode)
{
    overlay_state_t next = {overlay_type_t::MODE_WAVE, 0, MODE_WAVE_MS, 0, 255, 255, 255,
                            slide_dir_t::LEFT_TO_RIGHT};
    mode_color(mode, &next.r, &next.g, &next.b);
    start_overlay(overlay, next);
}

static void start_slide_overlay(overlay_state_t *overlay, uint8_t r, uint8_t g, uint8_t b, slide_dir_t dir)
{
    overlay_state_t next = {overlay_type_t::SLIDE, 0, SLIDE_OVERLAY_MS, 0, r, g, b, dir};
    start_overlay(overlay, next);
}

static void render_volume_overlay(sed_ws63::sk9822_led &led, const overlay_state_t &overlay, uint8_t brightness,
                                  uint8_t brightness_percent)
{
    uint8_t lit_cols = overlay.value == 0 ? 0 : (uint8_t)(((uint16_t)overlay.value * VISUAL_COLS + 99) / 100);
    if (lit_cols > VISUAL_COLS) {
        lit_cols = VISUAL_COLS;
    }

    uint8_t white = white_level_from_brightness(brightness_percent);
    for (uint8_t col = 0; col < VISUAL_COLS; col++) {
        if (col < lit_cols) {
            set_visual_column(led, col, white, white, white, brightness);
        } else {
            set_visual_column(led, col, 0, 0, 0, brightness);
        }
    }
}

static void render_mode_overlay(sed_ws63::sk9822_led &led, const overlay_state_t &overlay, uint8_t brightness,
                                uint8_t brightness_percent)
{
    float progress = (float)overlay.age_ms / (float)overlay.duration_ms;
    if (progress < 0.0f) {
        progress = 0.0f;
    }
    if (progress > 1.0f) {
        progress = 1.0f;
    }

    float eased = progress * progress * (3.0f - 2.0f * progress);
    float radius = eased * 4.55f;
    float width = 0.72f + sinf(progress * 3.14159265f) * 0.95f;
    float fade = 0.82f - progress * 0.22f;
    if (fade < 0.35f) {
        fade = 0.35f;
    }

    for (uint8_t col = 0; col < VISUAL_COLS; col++) {
        float dist = fabsf((float)col - 4.0f);
        float edge = fabsf(dist - radius);
        if (edge < width) {
            float strength = (1.0f - edge / width) * fade;
            uint8_t r, g, b;
            scale_overlay_color(overlay.r, overlay.g, overlay.b, strength, brightness_percent, &r, &g, &b);
            set_visual_column(led, col, r, g, b, brightness);
        } else if (progress < 0.38f && dist < 0.65f) {
            float center_strength = (0.38f - progress) / 0.38f * 0.42f;
            uint8_t r, g, b;
            scale_overlay_color(overlay.r, overlay.g, overlay.b, center_strength, brightness_percent, &r, &g, &b);
            set_visual_column(led, col, r, g, b, brightness);
        }
    }
}

static void render_slide_overlay(sed_ws63::sk9822_led &led, const overlay_state_t &overlay, uint8_t brightness,
                                 uint8_t brightness_percent)
{
    float progress = (float)(overlay.age_ms + FRAME_MS) / (float)overlay.duration_ms;
    if (progress < 0.0f) {
        progress = 0.0f;
    }
    if (progress > 1.0f) {
        progress = 1.0f;
    }

    float span = (float)VISUAL_COLS + 1.6f;
    float pos = (overlay.dir == slide_dir_t::LEFT_TO_RIGHT) ? (-0.8f + progress * span)
                                                            : ((float)VISUAL_COLS - 0.2f - progress * span);
    const float width = 1.35f;

    for (uint8_t col = 0; col < VISUAL_COLS; col++) {
        float dist = fabsf((float)col - pos);
        if (dist < width) {
            float strength = 1.0f - dist / width;
            uint8_t r, g, b;
            scale_overlay_color(overlay.r, overlay.g, overlay.b, strength, brightness_percent, &r, &g, &b);
            set_visual_column(led, col, r, g, b, brightness);
        }
    }
}

static void render_overlay(sed_ws63::sk9822_led &led, const overlay_state_t &overlay, uint8_t brightness,
                           uint8_t brightness_percent)
{
    if (overlay.type == overlay_type_t::NONE) {
        return;
    }

    switch (overlay.type) {
        case overlay_type_t::VOLUME:
            render_volume_overlay(led, overlay, brightness, brightness_percent);
            break;
        case overlay_type_t::MODE_WAVE:
            render_mode_overlay(led, overlay, brightness, brightness_percent);
            break;
        case overlay_type_t::SLIDE:
            render_slide_overlay(led, overlay, brightness, brightness_percent);
            break;
        default:
            break;
    }
}

static void advance_overlay(overlay_state_t *overlay)
{
    if (overlay->type == overlay_type_t::NONE) {
        return;
    }

    if (overlay->age_ms + FRAME_MS >= overlay->duration_ms) {
        overlay->type = overlay_type_t::NONE;
        overlay->age_ms = 0;
        return;
    }
    overlay->age_ms += FRAME_MS;
}

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
    overlay_state_t overlay = {overlay_type_t::NONE, 0, 0, 0, 0, 0, 0, slide_dir_t::LEFT_TO_RIGHT};
    spi_settings_t prev_settings = *get_spi_settings();

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

        spi_settings_t cur_settings = *get_spi_settings();

        uint8_t prev_hotspot = spi_get_hotspot(prev_settings.hotspot_network);
        uint8_t cur_hotspot = spi_get_hotspot(cur_settings.hotspot_network);
        uint8_t prev_network = spi_get_network(prev_settings.hotspot_network);
        uint8_t cur_network = spi_get_network(cur_settings.hotspot_network);

        bool volume_changed = cur_settings.volume != prev_settings.volume;
        bool mode_changed = cur_settings.mode != prev_settings.mode;
        bool hotspot_changed = cur_hotspot != prev_hotspot;
        bool network_changed = cur_network != prev_network;

        if (volume_changed) {
            start_volume_overlay(&overlay, cur_settings.volume);
        }
        if (mode_changed) {
            start_mode_overlay(&overlay, cur_settings.mode);
        }
        if (!mode_changed && hotspot_changed) {
            slide_dir_t dir = (cur_hotspot == SPI_HOTSPOT_ON) ? slide_dir_t::LEFT_TO_RIGHT : slide_dir_t::RIGHT_TO_LEFT;
            start_slide_overlay(&overlay, 255, 220, 0, dir);
        } else if (!mode_changed && network_changed) {
            slide_dir_t dir = (cur_network == SPI_NETWORK_CONN) ? slide_dir_t::LEFT_TO_RIGHT : slide_dir_t::RIGHT_TO_LEFT;
            start_slide_overlay(&overlay, 0, 80, 255, dir);
        }
        prev_settings = cur_settings;

        uint8_t led_brightness = cur_settings.brightness == 0 ? 0 : (uint8_t)(3 + cur_settings.brightness * 28 / 100);

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
            uint8_t pos = visual_col_from_index(i); /* 视觉位置 0..8, 左到右 */
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

        render_overlay(led, overlay, led_brightness, cur_settings.brightness);
        led.update();
        advance_overlay(&overlay);

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
