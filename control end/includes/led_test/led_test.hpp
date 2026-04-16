#ifndef __LED_TEST_HPP__
#define __LED_TEST_HPP__

extern "C" {
#include "soc_osal.h"
#include "gpio.h"
#include "pinctrl.h"
}

// 定义LED的基础功能
class LED_TEST {
public:
    // 构造函数：在创建变量的瞬间完成对变量的初始化
    LED_TEST(pin_t a = GPIO_02);

    // 添加一个函数实现电平转换，且开启时长
    void led_shine();
    void led_shine(int time);
    void led_extinguish();
    void led_extinguish(int time);

private:
public:
private:
    pin_t PIN = GPIO_02;
};

#endif