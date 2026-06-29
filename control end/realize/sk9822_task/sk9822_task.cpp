#include "sk9822_task.h"
#include "sk9822_led.hpp"
#include "spi_task.h"

static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    h %= 360;
    uint8_t region = h / 60;
    uint16_t remainder = (h - region * 60) * 255 / 60;

    uint8_t p = (uint8_t)((uint16_t)v * (255 - s) / 255);
    uint8_t q = (uint8_t)((uint16_t)v * (255 - (uint16_t)s * remainder / 255) / 255);
    uint8_t t = (uint8_t)((uint16_t)v * (255 - (uint16_t)s * (255 - remainder) / 255) / 255);

    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

static uint8_t get_band_at_distance(const audio_result_t *audio, uint8_t norm_dist)
{
    uint8_t band_idx = (uint16_t)norm_dist * 5 / 256;
    if (band_idx > 4) band_idx = 4;
    return audio->bands[band_idx];
}

void *sk9822_task(void *arg)
{
    unused(arg);

    sed_ws63::sk9822_led led;
    osal_printk("[SK9822] 音乐律动灯效任务启动\r\n");

    uint16_t tick = 0;
    uint8_t beat_flash = 0;
    int diag_cnt = 0;

    while (true) {
        const audio_result_t *audio = get_audio_result();

        tick++;

        if (++diag_cnt % 33 == 1) {
            osal_printk("[SK9822] audio: [%u %u %u %u %u] ov=%u bt=%u bf=%u tick=%u\r\n",
                        (unsigned)audio->bands[0], (unsigned)audio->bands[1], (unsigned)audio->bands[2],
                        (unsigned)audio->bands[3], (unsigned)audio->bands[4],
                        (unsigned)audio->overall, (unsigned)audio->beat,
                        (unsigned)beat_flash, (unsigned)tick);
        }

        uint16_t base_hue = (tick * 2) % 360;

        if (audio->beat && beat_flash == 0) {
            beat_flash = 4;
        }

        for (uint8_t i = 0; i < sed_ws63::sk9822_led::NUM_LEDS; i++) {
            uint8_t dist = (i < 9) ? (uint8_t)(8 - i) : (uint8_t)(i - 9);
            uint8_t norm_dist = (uint16_t)dist * 255 / 8;

            uint8_t energy = get_band_at_distance(audio, norm_dist);

            uint16_t hue = (base_hue + (uint16_t)dist * 20 + tick) % 360;

            uint8_t brightness = energy;
            if (brightness < 8) brightness = 8;
            if (beat_flash > 0) {
                brightness = 255;
            }

            uint8_t sat = (uint16_t)(255 - energy) * 100 / 255 + 155;
            if (beat_flash > 0) sat = 0;

            uint8_t r, g, b;
            hsv_to_rgb(hue, sat, brightness, &r, &g, &b);
            led.set_pixel(i, r, g, b, 15);
        }

        if (beat_flash > 0) beat_flash--;

        led.update();
        osal_msleep(30);
    }
    return NULL;
}
