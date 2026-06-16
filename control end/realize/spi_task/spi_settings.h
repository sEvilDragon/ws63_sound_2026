#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPI_CMD_QUERY   0
#define SPI_CMD_SYNC    127

#define SPI_HOTSPOT_OFF   3
#define SPI_HOTSPOT_ON    7
#define SPI_NETWORK_CONN  3
#define SPI_NETWORK_DISC  7

#define SPI_MODE_WIREED      0
#define SPI_MODE_SLE         63
#define SPI_MODE_DLNA        127
#define SPI_MODE_SLE_MIC     0x55
#define SPI_MODE_DLNA_NET    0xAA

#define SPI_SETTINGS_LEN  6

typedef struct {
    uint8_t cmd;
    uint8_t hotspot_network;
    uint8_t mode;
    uint8_t volume;
    uint8_t brightness;
    uint8_t bass;
} spi_settings_t;

static inline int spi_validate_cmd(uint8_t v)
{
    return (v == SPI_CMD_QUERY || v == SPI_CMD_SYNC);
}

static inline int spi_validate_hotspot_network(uint8_t v)
{
    uint8_t hi = (v >> 4) & 0x0F;
    uint8_t lo = v & 0x0F;
    return ((hi == SPI_HOTSPOT_OFF || hi == SPI_HOTSPOT_ON) &&
            (lo == SPI_NETWORK_CONN || lo == SPI_NETWORK_DISC));
}

static inline int spi_validate_mode(uint8_t v)
{
    return (v == SPI_MODE_WIREED || v == SPI_MODE_SLE || v == SPI_MODE_DLNA ||
            v == SPI_MODE_SLE_MIC || v == SPI_MODE_DLNA_NET);
}

static inline int spi_validate_percent(uint8_t v)
{
    return (v <= 100);
}

static inline int spi_validate_settings(const spi_settings_t *s)
{
    if (!s) return 0;
    return spi_validate_cmd(s->cmd) &&
           spi_validate_hotspot_network(s->hotspot_network) &&
           spi_validate_mode(s->mode) &&
           spi_validate_percent(s->volume) &&
           spi_validate_percent(s->brightness) &&
           spi_validate_percent(s->bass);
}

static inline uint8_t spi_make_hotspot_network(uint8_t hotspot, uint8_t network)
{
    return (uint8_t)(((hotspot & 0x0F) << 4) | (network & 0x0F));
}

static inline uint8_t spi_get_hotspot(uint8_t v)
{
    return (v >> 4) & 0x0F;
}

static inline uint8_t spi_get_network(uint8_t v)
{
    return v & 0x0F;
}

#ifdef __cplusplus
}
#endif
