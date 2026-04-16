#include "led_test.hpp"

void *led_test_task(void *arg)
{
    unused(arg);

    LED_TEST led{};
    while (true) {
        led.led_shine(1000);
        led.led_extinguish(1000);
    }
    return NULL;
}