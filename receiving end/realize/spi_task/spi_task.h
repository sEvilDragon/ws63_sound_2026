#pragma once

#include "spi_settings.h"

void *spi_master_task(void *arg);

void spi_settings_update_hotspot_network(uint8_t hotspot, uint8_t network);
void spi_settings_update_mode(uint8_t mode);
void spi_settings_update_volume(uint8_t volume);
void spi_settings_update_brightness(uint8_t brightness);
void spi_settings_update_bass(uint8_t bass);

const spi_settings_t *get_spi_settings();
