#include "iic.hpp"

namespace sed_ws63 {

// 默认激活iic1
iic_master::iic_master(pin_t scl = GPIO_16, pin_t sda = GPIO_15) : scl_pin(scl), sda_pin(sda)
{
    // 初始化iic
    iic_pin_init();
    uapi_i2c_master_init(I2C_BUS_0, 400000, 0); // 激活高速模式
}

void iic_master::iic_pin_init()
{
    // 配置scl和sda引脚为iic功能
    uapi_pin_set_mode(scl_pin, PIN_MODE_2);
    uapi_pin_set_mode(sda_pin, PIN_MODE_2);
}

void iic_master::iic_master_write(uint8_t *data, uint8_t len, uint16_t addr)
{
    // 基本iic写入代码
    i2c_data_t data_ = {0};
    data_.send_buf = data;
    data_.send_len = len;
    uapi_i2c_master_write(I2C_BUS_0, addr, &data_);
}

void iic_master::iic_master_read(uint8_t *data_tar, uint8_t len_tra, uint8_t *data_res, uint8_t len_res, uint16_t addr)
{
    // 基本iic读取代码
    i2c_data_t data_ = {0};
    data_.send_buf = data_tar;
    data_.send_len = len_tra;
    data_.receive_buf = data_res;
    data_.receive_len = len_res;
    uapi_i2c_master_writeread(I2C_BUS_0, addr, &data_);
}

}