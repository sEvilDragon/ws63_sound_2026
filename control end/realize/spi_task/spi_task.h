#pragma once

#include "spi_settings.h"

void *spi_slave_task(void *arg);

const spi_settings_t *get_spi_settings();
