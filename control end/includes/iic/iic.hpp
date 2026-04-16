#pragma once

extern "C" {
#include "pinctrl.h"
#include "i2c.h"
#include "soc_osal.h"
#include "app_init.h"
}

namespace sed_ws63 {
class iic_master {
public:
private:
    // 定义iic的引脚
    pin_t scl_pin;
    pin_t sda_pin;

public:
    iic_master(pin_t scl, pin_t sda);
    ~iic_master() = default;
    void iic_master_write(uint8_t *data, uint8_t len, uint16_t addr);
    void iic_master_read(uint8_t *data_tar, uint8_t len_tra, uint8_t *data_res, uint8_t len_res, uint16_t addr);

private:
    // iic相关激活函数
    void iic_pin_init();
};
} // namespace sed_ws63