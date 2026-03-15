#include "led_test.hpp"

LED_TEST::LED_TEST(pin_t a)
{
    // 强制转换为枚举值
    PIN = a;

    // 初始化引脚
    uapi_pin_init();
    uapi_gpio_init();

    // 调整引脚复用
    uapi_pin_set_mode(PIN, HAL_PIO_FUNC_GPIO); 
    uapi_gpio_set_dir(PIN, GPIO_DIRECTION_OUTPUT);
    // 默认使用高电平
    uapi_gpio_set_val(PIN, GPIO_LEVEL_HIGH);
}

void LED_TEST::led_shine()
{
    uapi_gpio_set_val(PIN, GPIO_LEVEL_HIGH);
}

void LED_TEST::led_shine(int time)
{
    uapi_gpio_set_val(PIN, GPIO_LEVEL_HIGH);
    osal_msleep(time);
}

void LED_TEST::led_extinguish()
{
    uapi_gpio_set_val(PIN, GPIO_LEVEL_LOW);
}

void LED_TEST::led_extinguish(int time)
{
    uapi_gpio_set_val(PIN, GPIO_LEVEL_LOW);
    osal_msleep(time);
}