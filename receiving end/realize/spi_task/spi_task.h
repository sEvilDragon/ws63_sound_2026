#pragma once

#include "spi_settings.h"
#include "audio_analyzer.hpp"

#define SPI_AUDIO_OFFSET  SPI_SETTINGS_LEN

void *spi_master_task(void *arg);

void spi_settings_update_hotspot_network(uint8_t hotspot, uint8_t network);
void spi_settings_update_mode(uint8_t mode);
void spi_settings_update_volume(uint8_t volume);
void spi_settings_update_brightness(uint8_t brightness);
void spi_settings_update_bass(uint8_t bass);
void spi_settings_update_tone(uint8_t tone);
void spi_settings_update_night(uint8_t enabled);

const spi_settings_t *get_spi_settings();
const char *spi_tone_name(uint8_t tone);
uint8_t spi_settings_effective_volume(const spi_settings_t *s);
uint8_t spi_settings_effective_bass(const spi_settings_t *s);

int spi_settings_load_from_nv(void);
void spi_settings_nv_flush_if_idle(void);
