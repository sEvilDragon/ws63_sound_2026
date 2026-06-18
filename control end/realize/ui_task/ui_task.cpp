#include "ui_task.h"
#include "ui_task.hpp"
#include "spi_task.h"

extern "C" {
#include "soc_osal.h"
#include "app_init.h"
}

namespace sed_ws63 {

static constexpr int LONG_PRESS_THRESHOLD = 100;
static constexpr int LONG_PRESS_REPEAT = 20;
// swipe 阈值已按 TTP_SLIDER_SCALE 放大: 需移动 ≥ 2 个逻辑档位且速度 > 5
static constexpr int SWIPE_THRESHOLD = 2 * TTP_SLIDER_SCALE;
static constexpr int SWIPE_SPEED_MIN = 5;
static constexpr int LONG_PRESS_SIDE_NONE = 0;
static constexpr int LONG_PRESS_SIDE_LEFT = 1;
static constexpr int LONG_PRESS_SIDE_RIGHT = 2;

// 滑条灵敏度: 手指从左滑到右 (700 缩放单位) 对应 value 变化量
// 50 = 一次全滑条约 ±50, 类似鼠标滚轮, 想更快就改大, 更慢就改小
static constexpr int SLIDER_SENSITIVITY = 50;

static constexpr uint8_t MODE_LIST[3] = {SPI_MODE_SLE, SPI_MODE_DLNA, SPI_MODE_WIREED};

static ui_state_t g_ui;

// 每帧锁存: ui_tick() 入口一次性读出并清零, 整个 tick 内复用本地副本
static uint8_t g_press_edge = 0;
static uint8_t g_release_edge = 0;

static int clamp100(int v)
{
    if (v < 0)
        return 0;
    if (v > 100)
        return 100;
    return v;
}

static void cycle_mode(int dir)
{
    spi_settings_t *s = (spi_settings_t *)get_spi_settings();
    int idx = -1;
    for (int i = 0; i < 3; i++) {
        if (MODE_LIST[i] == s->mode) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        idx = 0;
    idx = (idx + dir + 3) % 3;
    s->mode = MODE_LIST[idx];
    osal_printk("[UI] mode -> %s (%u)\r\n", mode_name(s->mode), (unsigned)s->mode);
}

static void set_value_with_log(ui_config_target_t target, int new_val)
{
    spi_settings_t *s = (spi_settings_t *)get_spi_settings();
    new_val = clamp100(new_val);

    uint8_t *p = nullptr;
    const char *name = nullptr;
    switch (target) {
        case UI_TARGET_VOLUME:
            p = &s->volume;
            name = "volume";
            break;
        case UI_TARGET_BRIGHTNESS:
            p = &s->brightness;
            name = "brightness";
            break;
        case UI_TARGET_BASS:
            p = &s->bass;
            name = "bass";
            break;
        default:
            return;
    }

    if ((int)*p != new_val) {
        *p = (uint8_t)new_val;
        osal_printk("[UI] %s -> %d\r\n", name, new_val);
    }
}

static int get_current_value(ui_config_target_t target)
{
    const spi_settings_t *s = get_spi_settings();
    switch (target) {
        case UI_TARGET_VOLUME:
            return s->volume;
        case UI_TARGET_BRIGHTNESS:
            return s->brightness;
        case UI_TARGET_BASS:
            return s->bass;
        default:
            return 0;
    }
}

static void toggle_hotspot(void)
{
    spi_settings_t *s = (spi_settings_t *)get_spi_settings();
    uint8_t hs = spi_get_hotspot(s->hotspot_network);
    uint8_t nw = spi_get_network(s->hotspot_network);
    uint8_t new_hs = (hs == SPI_HOTSPOT_ON) ? SPI_HOTSPOT_OFF : SPI_HOTSPOT_ON;
    s->hotspot_network = spi_make_hotspot_network(new_hs, nw);
    osal_printk("[UI] hotspot -> %s\r\n", new_hs == SPI_HOTSPOT_ON ? "ON" : "OFF");
}

static int detect_side(int pos)
{
    // pos is scaled (×TTP_SLIDER_SCALE), range 0..700
    if (pos >= 0 && pos < 4 * TTP_SLIDER_SCALE)
        return LONG_PRESS_SIDE_LEFT;
    if (pos >= 4 * TTP_SLIDER_SCALE && pos <= 7 * TTP_SLIDER_SCALE)
        return LONG_PRESS_SIDE_RIGHT;
    return LONG_PRESS_SIDE_NONE;
}

static void reset_long_press(void)
{
    g_ui.long_press.side = LONG_PRESS_SIDE_NONE;
    g_ui.long_press.ticks = 0;
}

static void reset_slider(void)
{
    g_ui.slider.active = false;
    g_ui.slider.last_pos = TTP_SLIDER_NO_POS;
    g_ui.slider.last_value = -1;
}

static void reset_swipe(void)
{
    g_ui.swipe.dispatched = false;
    g_ui.swipe.anchor_pos = 0;
    g_ui.swipe.anchor_valid = false;
}

static void handle_long_press_tick(int side, ui_config_target_t target)
{
    if (g_ui.long_press.side != side) {
        g_ui.long_press.side = (uint8_t)side;
        g_ui.long_press.ticks = 0;
        return;
    }

    g_ui.long_press.ticks++;
    if (g_ui.long_press.ticks < LONG_PRESS_THRESHOLD)
        return;

    int over = g_ui.long_press.ticks - LONG_PRESS_THRESHOLD;
    if (over == 0 || (over > 0 && (over % LONG_PRESS_REPEAT) == 0)) {
        int delta = (side == LONG_PRESS_SIDE_RIGHT) ? +1 : -1;
        int cur = get_current_value(target);
        set_value_with_log(target, cur + delta);
        g_ui.slider.last_value = clamp100(cur + delta);
    }
}

static void handle_main_mode(void)
{
    const ttp_state_t *t = ttp_get_state();

    if (g_press_edge & (1u << TTP_FUNC_IDX_C)) {
        toggle_hotspot();
        reset_long_press();
        return;
    }

    if (t->slider_pos != TTP_SLIDER_NO_POS) {
        // 新触碰: 上一轮未设 anchor 或已释放, 现在重新设定
        if (!g_ui.swipe.anchor_valid) {
            g_ui.swipe.anchor_pos = t->slider_pos;
            g_ui.swipe.anchor_valid = true;
            g_ui.swipe.dispatched = false;
        }
        int delta = t->slider_pos - g_ui.swipe.anchor_pos;
        int abs_delta = (delta < 0) ? -delta : delta;
        if (!g_ui.swipe.dispatched && abs_delta >= SWIPE_THRESHOLD && t->slider_speed > SWIPE_SPEED_MIN) {
            int dir = (delta > 0) ? +1 : -1;
            cycle_mode(dir);
            g_ui.swipe.dispatched = true;
            reset_long_press();
            return;
        }
    } else {
        // 滑条释放: 清掉 anchor_valid, 下次触碰必须重新设定 anchor
        // 这样 "点左 + 释放 + 点右" 不会被当成从左到右的滑动
        reset_swipe();
    }

    if (t->slider_pos == TTP_SLIDER_NO_POS) {
        reset_long_press();
        return;
    }

    uint16_t full = t->raw_state;
    int pop = 0;
    for (int i = 0; i < 16; i++)
        if (full & (1u << i))
            pop++;

    if (pop == 1 && !g_ui.swipe.dispatched) {
        int side = detect_side(t->slider_pos);
        if (side != LONG_PRESS_SIDE_NONE) {
            handle_long_press_tick(side, UI_TARGET_VOLUME);
            return;
        }
    }
    reset_long_press();
}

static void handle_config_mode(void)
{
    const ttp_state_t *t = ttp_get_state();

    if (g_press_edge & (1u << TTP_FUNC_IDX_B)) {
        g_ui.target = (ui_config_target_t)(((int)g_ui.target + 1) % UI_TARGET_COUNT);
        osal_printk("[UI] config target -> %s\r\n", target_name(g_ui.target));
        reset_long_press();
        reset_slider();
        reset_swipe();
        return;
    }

    if (g_press_edge & (1u << TTP_FUNC_IDX_C)) {
        toggle_hotspot();
        reset_long_press();
        return;
    }

    if (t->slider_pos != TTP_SLIDER_NO_POS) {
        // 首次触碰: 记录锚点, 不产生增量
        if (!g_ui.slider.active) {
            g_ui.slider.last_pos = t->slider_pos;
            g_ui.slider.active = true;
            reset_long_press();
            return;
        }

        int delta = t->slider_pos - g_ui.slider.last_pos;
        if (delta != 0) {
            if (g_ui.target == UI_TARGET_MODE) {
                // mode 只有 3 个选项, 仍然用绝对位置映射
                int mode_idx = (t->slider_pos / TTP_SLIDER_SCALE) % 3;
                uint8_t new_mode = MODE_LIST[mode_idx];
                spi_settings_t *s = (spi_settings_t *)get_spi_settings();
                if (s->mode != new_mode) {
                    s->mode = new_mode;
                    osal_printk("[UI] mode -> %s (%u) (slider pos %d.%02d)\r\n", mode_name(s->mode), (unsigned)s->mode,
                                t->slider_pos / TTP_SLIDER_SCALE, t->slider_pos % TTP_SLIDER_SCALE);
                }
            } else {
                // ★ 鼠标滚轮式相对滑动 ★
                // delta 单位: 缩放值 (×100); 700 = 一个完整 8-pad 滑程
                // value_delta = delta × SENSITIVITY / 700
                int value_delta = (delta * SLIDER_SENSITIVITY) / (7 * TTP_SLIDER_SCALE);
                if (value_delta != 0) {
                    int cur = get_current_value(g_ui.target);
                    int new_val = clamp100(cur + value_delta);
                    set_value_with_log(g_ui.target, new_val);
                    g_ui.slider.last_value = new_val;
                }
            }
            g_ui.slider.last_pos = t->slider_pos;
            reset_long_press();
            return;
        }

        // 位置不变: 检查单指长按微调 (±1)
        uint16_t full = t->raw_state;
        int pop = 0;
        for (int i = 0; i < 16; i++)
            if (full & (1u << i))
                pop++;

        if (pop == 1 && g_ui.target != UI_TARGET_MODE) {
            int side = detect_side(t->slider_pos);
            if (side != LONG_PRESS_SIDE_NONE) {
                handle_long_press_tick(side, g_ui.target);
                return;
            }
        }
        reset_long_press();
    } else {
        // 手指离开: 重置状态, 下次触碰从新锚点开始计增量
        reset_long_press();
        reset_slider();
    }
}

static void ui_tick(void)
{
    // 一次性消费本周期所有边沿, 后续整段逻辑复用同一份本地副本
    g_press_edge = ttp_consume_press_latch();
    g_release_edge = ttp_consume_release_latch();

    const ttp_state_t *t = ttp_get_state();

    if (g_press_edge & (1u << TTP_FUNC_IDX_A)) {
        if (g_ui.mode == UI_MAIN) {
            g_ui.mode = UI_CONFIG;
            g_ui.target = UI_TARGET_MODE;
            osal_printk("[UI] -> CONFIG mode (target=%s)\r\n", target_name(g_ui.target));
        } else {
            g_ui.mode = UI_MAIN;
            osal_printk("[UI] -> MAIN mode\r\n");
        }
        reset_long_press();
        reset_slider();
        reset_swipe();
        return;
    }

    if (g_ui.mode == UI_MAIN)
        handle_main_mode();
    else
        handle_config_mode();
}

} // namespace sed_ws63

void *ui_task(void *arg)
{
    (void)arg;

    const spi_settings_t *s0 = get_spi_settings();
    osal_printk("[UI] task started. mode=%s vol=%u bri=%u bass=%u\r\n", sed_ws63::mode_name(s0->mode),
                (unsigned)s0->volume, (unsigned)s0->brightness, (unsigned)s0->bass);

    while (true) {
        sed_ws63::ui_tick();
        osal_msleep(5);
    }
    return nullptr;
}
