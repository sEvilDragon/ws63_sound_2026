#include "ui_task.h"
#include "ui_task.hpp"
#include "spi_task.h"
#include "ttp_task.h"
#include "voice_task.h"

extern "C" {
#include "app_init.h"
#include "soc_osal.h"
}

namespace sed_ws63 {

static constexpr uint8_t MODE_LIST[] = {
    SPI_MODE_WIREED, // 无模式
    SPI_MODE_SLE,
    SPI_MODE_DLNA,
};
static constexpr int MODE_COUNT = sizeof(MODE_LIST) / sizeof(MODE_LIST[0]);
static constexpr int SLIDER_SENSITIVITY = 50;

static ui_target_t g_target = UI_TARGET_MODE;
static int g_last_slider_pos = TTP_SLIDER_NO_POS;
static bool g_mode_changed_in_gesture = false;

static int clamp_percent(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return value;
}

static void reset_slider_tracking(void)
{
    g_last_slider_pos = TTP_SLIDER_NO_POS;
    g_mode_changed_in_gesture = false;
}

static void cycle_mode(int direction)
{
    const spi_settings_t *settings = get_spi_settings();
    int index = 0;
    for (int i = 0; i < MODE_COUNT; i++) {
        if (MODE_LIST[i] == settings->mode) {
            index = i;
            break;
        }
    }

    index = (index + direction + MODE_COUNT) % MODE_COUNT;
    spi_settings_update_mode(MODE_LIST[index]);
    osal_printk("[UI] slider: mode=%s direction=%s\r\n", mode_name(MODE_LIST[index]),
                direction > 0 ? "right" : "left");
}

static void select_next_target(void)
{
    g_target = (ui_target_t)(((int)g_target + 1) % UI_TARGET_COUNT);
    reset_slider_tracking();

    switch (g_target) {
        case UI_TARGET_MODE:
            voice_request_mode_selection_prompt();
            break;
        case UI_TARGET_VOLUME:
            voice_request_volume_adjust_prompt();
            break;
        case UI_TARGET_BRIGHTNESS:
            voice_request_brightness_adjust_prompt();
            break;
        default:
            break;
    }
    osal_printk("[UI] key A: target=%s\r\n", target_name(g_target));
}

static void toggle_hotspot(void)
{
    const spi_settings_t *settings = get_spi_settings();
    uint8_t hotspot = spi_get_hotspot(settings->hotspot_network);
    uint8_t network = spi_get_network(settings->hotspot_network);
    uint8_t next = (hotspot == SPI_HOTSPOT_ON) ? SPI_HOTSPOT_OFF : SPI_HOTSPOT_ON;

    spi_settings_update_hotspot_network(next, network);
    bool is_on = spi_get_hotspot(get_spi_settings()->hotspot_network) == SPI_HOTSPOT_ON;
    if (is_on) {
        voice_request_hotspot_on_prompt();
    } else {
        voice_request_hotspot_off_prompt();
    }
    osal_printk("[UI] key B: hotspot=%s\r\n", is_on ? "ON" : "OFF");
}

static void toggle_network(void)
{
    const spi_settings_t *settings = get_spi_settings();
    uint8_t hotspot = spi_get_hotspot(settings->hotspot_network);
    uint8_t network = spi_get_network(settings->hotspot_network);
    uint8_t next = (network == SPI_NETWORK_CONN) ? SPI_NETWORK_DISC : SPI_NETWORK_CONN;

    spi_settings_update_hotspot_network(hotspot, next);
    bool is_on = spi_get_network(get_spi_settings()->hotspot_network) == SPI_NETWORK_CONN;
    if (is_on) {
        voice_request_network_on_prompt();
    } else {
        voice_request_network_off_prompt();
    }
    osal_printk("[UI] key C: network=%s\r\n", is_on ? "ON" : "OFF");
}

static void handle_slider(void)
{
    int pos = ttp_get_state()->slider_pos;
    if (pos == TTP_SLIDER_NO_POS) {
        reset_slider_tracking();
        return;
    }

    if (g_last_slider_pos == TTP_SLIDER_NO_POS) {
        g_last_slider_pos = pos;
        return;
    }

    int delta = pos - g_last_slider_pos;
    if (delta == 0) {
        return;
    }
    g_last_slider_pos = pos;

    switch (g_target) {
        case UI_TARGET_MODE:
            if (!g_mode_changed_in_gesture) {
                cycle_mode(delta > 0 ? 1 : -1);
                g_mode_changed_in_gesture = true;
            }
            break;
        case UI_TARGET_VOLUME: {
            int value_delta = delta * SLIDER_SENSITIVITY /
                              ((TTP_SLIDER_PADS_COUNT - 1) * TTP_SLIDER_SCALE);
            if (value_delta != 0) {
                int volume = clamp_percent((int)get_spi_settings()->volume + value_delta);
                spi_settings_update_volume((uint8_t)volume);
                osal_printk("[UI] slider: volume=%u delta=%d\r\n", (unsigned)volume, value_delta);
            }
            break;
        }
        case UI_TARGET_BRIGHTNESS: {
            int value_delta = delta * SLIDER_SENSITIVITY /
                              ((TTP_SLIDER_PADS_COUNT - 1) * TTP_SLIDER_SCALE);
            if (value_delta != 0) {
                int brightness = clamp_percent((int)get_spi_settings()->brightness + value_delta);
                spi_settings_update_brightness((uint8_t)brightness);
                osal_printk("[UI] slider: brightness=%u delta=%d\r\n", (unsigned)brightness, value_delta);
            }
            break;
        }
        default:
            break;
    }
}

static void ui_tick(void)
{
    uint8_t press = ttp_consume_press_latch();

    if (press & (1u << TTP_FUNC_IDX_A)) {
        select_next_target();
        return;
    }
    if (press & (1u << TTP_FUNC_IDX_B)) {
        toggle_hotspot();
        return;
    }
    if (press & (1u << TTP_FUNC_IDX_C)) {
        toggle_network();
        return;
    }

    handle_slider();
}

} // namespace sed_ws63

void *ui_task(void *arg)
{
    (void)arg;

    const spi_settings_t *settings = get_spi_settings();
    osal_printk("[UI] started: target=%s mode=%s volume=%u brightness=%u hotspot=%s network=%s\r\n",
                sed_ws63::target_name(sed_ws63::UI_TARGET_MODE), sed_ws63::mode_name(settings->mode),
                (unsigned)settings->volume, (unsigned)settings->brightness,
                spi_get_hotspot(settings->hotspot_network) == SPI_HOTSPOT_ON ? "ON" : "OFF",
                spi_get_network(settings->hotspot_network) == SPI_NETWORK_CONN ? "ON" : "OFF");

    while (true) {
        sed_ws63::ui_tick();
        nv_flush_if_idle();
        osal_msleep(5);
    }
    return nullptr;
}
