#pragma once

#include "ttp_task.h"
#include "spi_settings.h"
#include <stdint.h>

namespace sed_ws63 {

enum ui_mode_t {
    UI_MAIN   = 0,
    UI_CONFIG = 1,
};

enum ui_config_target_t {
    UI_TARGET_MODE       = 0,
    UI_TARGET_VOLUME     = 1,
    UI_TARGET_BRIGHTNESS = 2,
    UI_TARGET_BASS       = 3,
    UI_TARGET_COUNT      = 4,
};

struct ui_long_press_t {
    uint8_t side;
    int     ticks;
};

struct ui_swipe_t {
    int     anchor_pos;
    bool    dispatched;
    bool    anchor_valid;   // 本轮触碰是否已设定 anchor; 释放时清 0
};

struct ui_slider_pos_t {
    int last_pos;
    int last_value;
    bool active;
};

struct ui_state_t {
    ui_mode_t        mode;
    ui_config_target_t target;
    ui_long_press_t  long_press;
    ui_swipe_t       swipe;
    ui_slider_pos_t  slider;
};

inline const char *mode_name(uint8_t m)
{
    switch (m) {
        case SPI_MODE_WIREED: return "wired";
        case SPI_MODE_SLE:    return "SLE";
        case SPI_MODE_DLNA:   return "DLNA";
        default:              return "?";
    }
}

inline const char *target_name(ui_config_target_t t)
{
    switch (t) {
        case UI_TARGET_MODE:       return "mode";
        case UI_TARGET_VOLUME:     return "volume";
        case UI_TARGET_BRIGHTNESS: return "brightness";
        case UI_TARGET_BASS:       return "bass";
        default:                   return "?";
    }
}

} // namespace sed_ws63
