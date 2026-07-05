#include "iic.hpp"
#include "gpio.h"

namespace sed_ws63 {

// 默认激活iic1
iic_master::iic_master(pin_t scl, pin_t sda, bool init_bus) : scl_pin(scl), sda_pin(sda)
{
    if (!init_bus) {
        return;
    }
    // 初始化iic
    iic_pin_init();
    errcode_t ret = uapi_i2c_master_init(I2C_BUS_1, 100000, 0); // GPIO_15/16 + PIN_MODE_2 → I2C_BUS_1
    if (ret != ERRCODE_SUCC) {
        osal_printk("I2C 总线初始化失败, ret=0x%x\r\n", ret);
    }
}

void iic_master::iic_pin_init()
{
    // 配置scl和sda引脚为iic功能
    uapi_pin_set_mode(scl_pin, PIN_MODE_2);
    uapi_pin_set_mode(sda_pin, PIN_MODE_2);
    // 开启内部上拉（若硬件已有外部上拉电阻，可注释掉）
    uapi_pin_set_pull(scl_pin, PIN_PULL_TYPE_UP);
    uapi_pin_set_pull(sda_pin, PIN_PULL_TYPE_UP);
}

bool iic_master::iic_master_write(uint8_t *data, uint8_t len, uint16_t addr)
{
    // 基本iic写入代码
    i2c_data_t data_{};
    data_.send_buf = data;
    data_.send_len = len;
    errcode_t err = uapi_i2c_master_write(I2C_BUS_1, addr, &data_);
    if (err != 0) {
        osal_printk("I2C 写入错误: %d\n", err);
        return false;
    }
    return true;
}

bool iic_master::iic_master_read(uint8_t *data_tar, uint8_t len_tra, uint8_t *data_res, uint8_t len_res, uint16_t addr)
{
    // 基本iic读取代码
    i2c_data_t data_{};
    data_.send_buf = data_tar;
    data_.send_len = len_tra;
    data_.receive_buf = data_res;
    data_.receive_len = len_res;
    errcode_t err = uapi_i2c_master_writeread(I2C_BUS_1, addr, &data_);
    if (err != 0) {
        osal_printk("I2C 读取错误: %d\n", err);
        return false;
    }
    return true;
}


bool iic_master::iic_master_read_only(uint8_t *data_res, uint8_t len_res, uint16_t addr)
{
    i2c_data_t data_{};
    data_.receive_buf = data_res;
    data_.receive_len = len_res;
    errcode_t err = uapi_i2c_master_read(I2C_BUS_1, addr, &data_);
    if (err != 0) {
        osal_printk("I2C read error: %d\n", err);
        return false;
    }
    return true;
}

} // namespace sed_ws63
