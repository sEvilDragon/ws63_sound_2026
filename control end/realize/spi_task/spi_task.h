#pragma once

#include "spi_settings.h"

#define SPI_AUDIO_OFFSET  SPI_SETTINGS_LEN

typedef struct {
    uint8_t bands[5];
    uint8_t overall;
    uint8_t beat;
} audio_result_t;

void *spi_slave_task(void *arg);

const spi_settings_t *get_spi_settings();
const audio_result_t *get_audio_result();

void spi_settings_update_hotspot_network(uint8_t hotspot, uint8_t network);
void spi_settings_update_mode(uint8_t mode);
void spi_settings_update_volume(uint8_t volume);
void spi_settings_update_brightness(uint8_t brightness);
void spi_settings_update_bass(uint8_t bass);
void spi_settings_update_tone(uint8_t tone);
void spi_settings_update_night(uint8_t enabled);

/**
 * @brief 上电时从 NV 恢复上次保存的 SPI 配置。
 *        应在 uapi_nv_init() 之后、任何 UI 操作之前调用。
 */
void nv_load_settings(void);

/**
 * @brief 标记配置已变更（仅设置脏标志，不立即写 Flash）。
 *        每次持久化配置字段变更后调用。
 *        实际写入由 nv_flush_if_idle() 在空闲 5 秒后触发。
 */
void nv_mark_dirty(void);

/**
 * @brief 若配置已变更且距上次变更超过 5 秒，则写入 NV 持久化。
 *        应在主循环中周期性调用（如每 5ms）。
 */
void nv_flush_if_idle(void);

/**
 * @brief 立即写入 NV，不等防抖。供关机/测试使用。
 */
void nv_flush_now(void);
