#include "sk9822_task.h"
#include "sk9822_led.hpp"

static const uint8_t COLOR_WHEEL[][3] = {
    {255,   0,   0}, // Red
    {255, 127,   0}, // Orange
    {255, 255,   0}, // Yellow
    {  0, 255,   0}, // Green
    {  0, 255, 255}, // Cyan
    {  0,   0, 255}, // Blue
    {127,   0, 255}, // Purple
    {255,   0, 255}, // Magenta
};

void *sk9822_task(void *arg)
{
    unused(arg);

    sed_ws63::sk9822_led led;
    uint8_t color_count = sizeof(COLOR_WHEEL) / sizeof(COLOR_WHEEL[0]);
    uint8_t shift = 0;

    while (true) {
        for (uint8_t i = 0; i < sed_ws63::sk9822_led::NUM_LEDS; i++) {
            uint8_t color_idx = (i + shift) % color_count;
            led.set_pixel(i, COLOR_WHEEL[color_idx][0], COLOR_WHEEL[color_idx][1], COLOR_WHEEL[color_idx][2], 5);
        }
        led.update();

        shift++;
        if (shift >= color_count) shift = 0;

        osal_msleep(500);
    }
    return NULL;
}
