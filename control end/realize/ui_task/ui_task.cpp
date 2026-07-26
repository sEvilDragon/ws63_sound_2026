#include "ui_task.h"
#include "ui_task.hpp"
#include "sk9822_task.h"
#include "spi_task.h"
#include "ttp_task.h"

extern "C" {
#include "app_init.h"
#include "soc_osal.h"
#include "systick.h"
}

namespace sed_ws63 {

static constexpr uint8_t MODE_LIST[] = {
    SPI_MODE_WIREED,
    SPI_MODE_SLE,
    SPI_MODE_DLNA,
};
static constexpr int MODE_COUNT = sizeof(MODE_LIST) / sizeof(MODE_LIST[0]);

static constexpr uint32_t LONG_PRESS_MS = 800;
static constexpr uint32_t MODE_REPEAT_MS = 1200;
static constexpr uint32_t VOLUME_REPEAT_MS = 100;
static constexpr uint32_t VOLUME_EDGE_PAUSE_MS = 800;
static constexpr uint8_t VOLUME_STEP = 2;

enum class volume_hold_phase_t : uint8_t {
    UP = 0,
    PAUSE_AT_MAX,
    DOWN,
    STOPPED,
};

static ui_target_t g_target = UI_TARGET_MODE;
static bool g_button_was_pressed = false;
static bool g_long_press_active = false;
static uint64_t g_press_started_ms = 0;
static uint64_t g_next_action_ms = 0;
static volume_hold_phase_t g_volume_phase = volume_hold_phase_t::STOPPED;

static void cycle_mode(void)
{
    const spi_settings_t *settings = get_spi_settings();
    int index = 0;
    for (int i = 0; i < MODE_COUNT; i++) {
        if (MODE_LIST[i] == settings->mode) {
            index = i;
            break;
        }
    }

    index = (index + 1) % MODE_COUNT;
    spi_settings_update_mode(MODE_LIST[index]);
    osal_printk("[UI] hold: mode=%s\r\n", mode_name(MODE_LIST[index]));
}

static void toggle_hotspot(void)
{
    const spi_settings_t *settings = get_spi_settings();
    uint8_t hotspot = spi_get_hotspot(settings->hotspot_network);
    uint8_t network = spi_get_network(settings->hotspot_network);
    uint8_t next = (hotspot == SPI_HOTSPOT_ON) ? SPI_HOTSPOT_OFF : SPI_HOTSPOT_ON;

    spi_settings_update_hotspot_network(next, network);
    osal_printk("[UI] hold: hotspot=%s\r\n", next == SPI_HOTSPOT_ON ? "ON" : "OFF");
}

static void toggle_network(void)
{
    const spi_settings_t *settings = get_spi_settings();
    uint8_t hotspot = spi_get_hotspot(settings->hotspot_network);
    uint8_t network = spi_get_network(settings->hotspot_network);
    uint8_t next = (network == SPI_NETWORK_CONN) ? SPI_NETWORK_DISC : SPI_NETWORK_CONN;

    spi_settings_update_hotspot_network(hotspot, next);
    osal_printk("[UI] hold: network=%s\r\n", next == SPI_NETWORK_CONN ? "ON" : "OFF");
}

static void select_next_target(void)
{
    g_target = (ui_target_t)(((int)g_target + 1) % UI_TARGET_COUNT);
    sk9822_show_control_target((uint8_t)g_target);
    osal_printk("[UI] click: target=%s\r\n", target_name(g_target));
}

static void update_volume_hold(uint64_t now)
{
    if (g_volume_phase == volume_hold_phase_t::STOPPED || now < g_next_action_ms) {
        return;
    }

    uint8_t volume = get_spi_settings()->volume;
    switch (g_volume_phase) {
        case volume_hold_phase_t::UP: {
            uint16_t next = (uint16_t)volume + VOLUME_STEP;
            if (next >= 100) {
                next = 100;
                g_volume_phase = volume_hold_phase_t::PAUSE_AT_MAX;
                g_next_action_ms = now + VOLUME_EDGE_PAUSE_MS;
            } else {
                g_next_action_ms = now + VOLUME_REPEAT_MS;
            }
            spi_settings_update_volume((uint8_t)next);
            break;
        }
        case volume_hold_phase_t::PAUSE_AT_MAX:
            g_volume_phase = volume_hold_phase_t::DOWN;
            g_next_action_ms = now;
            break;
        case volume_hold_phase_t::DOWN: {
            uint8_t next = volume > VOLUME_STEP ? (uint8_t)(volume - VOLUME_STEP) : 0;
            spi_settings_update_volume(next);
            if (next == 0) {
                g_volume_phase = volume_hold_phase_t::STOPPED;
            } else {
                g_next_action_ms = now + VOLUME_REPEAT_MS;
            }
            break;
        }
        default:
            break;
    }
}

static void begin_long_press(uint64_t now)
{
    g_long_press_active = true;
    osal_printk("[UI] long press: target=%s\r\n", target_name(g_target));

    switch (g_target) {
        case UI_TARGET_MODE:
            cycle_mode();
            g_next_action_ms = now + MODE_REPEAT_MS;
            break;
        case UI_TARGET_VOLUME:
            if (get_spi_settings()->volume >= 100) {
                g_volume_phase = volume_hold_phase_t::PAUSE_AT_MAX;
                g_next_action_ms = now + VOLUME_EDGE_PAUSE_MS;
            } else {
                g_volume_phase = volume_hold_phase_t::UP;
                g_next_action_ms = now;
                update_volume_hold(now);
            }
            break;
        case UI_TARGET_NETWORK:
            toggle_network();
            break;
        case UI_TARGET_HOTSPOT:
            toggle_hotspot();
            break;
        default:
            break;
    }
}

static void continue_long_press(uint64_t now)
{
    switch (g_target) {
        case UI_TARGET_MODE:
            if (now >= g_next_action_ms) {
                cycle_mode();
                g_next_action_ms = now + MODE_REPEAT_MS;
            }
            break;
        case UI_TARGET_VOLUME:
            update_volume_hold(now);
            break;
        default:
            break;
    }
}

static void ui_tick(void)
{
    uint64_t now = uapi_systick_get_ms();
    bool pressed = ttp_get_state()->button_pressed != 0;

    if (pressed && !g_button_was_pressed) {
        g_button_was_pressed = true;
        g_long_press_active = false;
        g_volume_phase = volume_hold_phase_t::STOPPED;
        g_press_started_ms = now;
    }

    if (pressed) {
        if (!g_long_press_active && now - g_press_started_ms >= LONG_PRESS_MS) {
            begin_long_press(now);
        } else if (g_long_press_active) {
            continue_long_press(now);
        }
        return;
    }

    if (g_button_was_pressed) {
        if (!g_long_press_active) {
            select_next_target();
        }
        g_button_was_pressed = false;
        g_long_press_active = false;
        g_volume_phase = volume_hold_phase_t::STOPPED;
    }
}

} // namespace sed_ws63

void *ui_task(void *arg)
{
    (void)arg;

    const spi_settings_t *settings = get_spi_settings();
    osal_printk("[UI] started: target=%s mode=%s volume=%u hotspot=%s network=%s\r\n",
                sed_ws63::target_name(sed_ws63::UI_TARGET_MODE), sed_ws63::mode_name(settings->mode),
                (unsigned)settings->volume,
                spi_get_hotspot(settings->hotspot_network) == SPI_HOTSPOT_ON ? "ON" : "OFF",
                spi_get_network(settings->hotspot_network) == SPI_NETWORK_CONN ? "ON" : "OFF");

    sk9822_show_control_target((uint8_t)sed_ws63::UI_TARGET_MODE);

    while (true) {
        sed_ws63::ui_tick();
        nv_flush_if_idle();
        osal_msleep(5);
    }
    return nullptr;
}
