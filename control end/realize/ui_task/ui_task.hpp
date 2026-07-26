#pragma once

#include "spi_settings.h"
#include <stdint.h>

namespace sed_ws63 {

enum ui_target_t {
    UI_TARGET_MODE = 0,
    UI_TARGET_VOLUME,
    UI_TARGET_NETWORK,
    UI_TARGET_HOTSPOT,
    UI_TARGET_COUNT,
};

inline const char *mode_name(uint8_t mode)
{
    switch (mode) {
        case SPI_MODE_WIREED: return "none";
        case SPI_MODE_SLE:    return "SLE";
        case SPI_MODE_DLNA:   return "DLNA";
        default:              return "?";
    }
}

inline const char *target_name(ui_target_t target)
{
    switch (target) {
        case UI_TARGET_MODE:    return "mode";
        case UI_TARGET_VOLUME:  return "volume";
        case UI_TARGET_NETWORK: return "network";
        case UI_TARGET_HOTSPOT: return "hotspot";
        default:                return "?";
    }
}

} // namespace sed_ws63
