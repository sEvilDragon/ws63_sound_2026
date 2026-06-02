#include "app_init.h"
#include "soc_osal.h"
#include "led_test.h"
#include "cs43131.hpp"

void *cs43131_task(void *arg)
{
    (void)arg; // 目前不使用参数，显式丢弃以消除 unused-but-set-parameter 警告
    sed_ws63::cs43131 cs43131_instance;
    osal_printk("CS43131 initialization complete, entering main loop.\n");
    while (true) {
        osal_msleep(1000);
    }
    return nullptr;
}

void app_entry(void)
{
    // // 初始化一个iic实例进行iic通讯测试
    // sed_ws63::iic_master iic_instance(GPIO_16, GPIO_15);

    // // ========== I2C 通讯诊断测试 ==========
    // // 目的：确认 I2C 总线号是否正确（I2C_BUS_0 vs I2C_BUS_1）
    // // 方法：向 AHT20 (地址 0x38) 读取 1 字节状态寄存器
    // {
    //     const uint16_t kAht20Addr = 0x38;   // AHT20 7-bit 地址（WS63 API 不需要左移）
    //     uint8_t rx_byte = 0xFF;
    //     i2c_data_t data = {0};
    //     data.receive_buf = &rx_byte;
    //     data.receive_len = 1;

    //     // --- 测试 I2C_BUS_0（当前 iic.cpp 使用的总线）---
    //     osal_printk("\n=== [TEST] I2C_BUS_0 (GPIO_16=SCL, GPIO_15=SDA) ===\r\n");
    //     errcode_t ret0 = uapi_i2c_master_init(I2C_BUS_0, 100000, 0);
    //     osal_printk("  init:  ret=0x%x %s\r\n", ret0,
    //                  (ret0 == ERRCODE_SUCC) ? "OK" : "FAIL");

    //     rx_byte = 0xFF;
    //     ret0 = uapi_i2c_master_read(I2C_BUS_0, kAht20Addr, &data);
    //     osal_printk("  read:  ret=0x%x rx=0x%02x %s\r\n", ret0, rx_byte,
    //                  (ret0 == ERRCODE_SUCC) ? "OK" : "FAIL");

    //     // --- 测试 I2C_BUS_1（官方 OLED demo 使用的总线）---
    //     osal_printk("=== [TEST] I2C_BUS_1 (GPIO_16=SCL, GPIO_15=SDA) ===\r\n");
    //     // 重新配置引脚（确保引脚状态正确）
    //     uapi_pin_set_mode(GPIO_16, PIN_MODE_2);
    //     uapi_pin_set_mode(GPIO_15, PIN_MODE_2);
    //     uapi_pin_set_pull(GPIO_16, PIN_PULL_TYPE_UP);
    //     uapi_pin_set_pull(GPIO_15, PIN_PULL_TYPE_UP);

    //     errcode_t ret1 = uapi_i2c_master_init(I2C_BUS_1, 100000, 0);
    //     osal_printk("  init:  ret=0x%x %s\r\n", ret1,
    //                  (ret1 == ERRCODE_SUCC) ? "OK" : "FAIL");

    //     rx_byte = 0xFF;
    //     ret1 = uapi_i2c_master_read(I2C_BUS_1, kAht20Addr, &data);
    //     osal_printk("  read:  ret=0x%x rx=0x%02x %s\r\n", ret1, rx_byte,
    //                  (ret1 == ERRCODE_SUCC) ? "OK" : "FAIL");

    //     // --- 用 writeread 方式再测 I2C_BUS_1（模拟真实寄存器访问）---
    //     osal_printk("=== [TEST] I2C_BUS_1 writeread (write 0xAC cmd) ===\r\n");
    //     uint8_t measure_cmd[3] = {0xAC, 0x33, 0x00};
    //     uint8_t rx_buf[6] = {0};
    //     i2c_data_t data_wr = {0};
    //     data_wr.send_buf = measure_cmd;
    //     data_wr.send_len = 3;
    //     data_wr.receive_buf = rx_buf;
    //     data_wr.receive_len = 0;  // 只写不读，测试是否有 ACK
    //     errcode_t ret_wr = uapi_i2c_master_write(I2C_BUS_1, kAht20Addr, &data_wr);
    //     osal_printk("  write(0xAC): ret=0x%x %s\r\n", ret_wr,
    //                  (ret_wr == ERRCODE_SUCC) ? "OK(ACK!)" : "FAIL(no ACK)");

    //     osal_printk("=== I2C 诊断结束 ===\r\n\n");
    // }
    // // ========== 诊断测试结束 ==========

    // ========== CS43131 I2C 地址扫描 (0x30~0x33) ==========
    // CS43131 的 I2C 地址由 ADR0/ADR1 引脚硬件 strap 决定:
    //   0x30 (ADR1=GND, ADR0=GND), 0x31, 0x32, 0x33 (ADR1=VCP,ADR0=VCP)
    // 先确认芯片在哪个地址上应答
    uapi_pin_set_mode(GPIO_16, PIN_MODE_2);
    uapi_pin_set_mode(GPIO_15, PIN_MODE_2);
    uapi_pin_set_pull(GPIO_16, PIN_PULL_TYPE_UP);
    uapi_pin_set_pull(GPIO_15, PIN_PULL_TYPE_UP);
    uapi_i2c_master_init(I2C_BUS_1, 100000, 0);

    // 确保 CS43131 的 RST 引脚为高（不被浮空误拉低）
    uapi_pin_set_mode(GPIO_10, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(GPIO_10, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(GPIO_10, GPIO_LEVEL_HIGH);

    osal_printk("=== CS43131 I2C Address Scan ===\r\n");
    for (uint16_t addr = 0x30; addr <= 0x33; ++addr) {
        i2c_data_t probe = {0};
        uint8_t rx = 0;
        probe.receive_buf = &rx;
        probe.receive_len = 1;
        errcode_t ret = uapi_i2c_master_read(I2C_BUS_1, addr, &probe);
        osal_printk("  0x%02X: ret=0x%08X %s\r\n", addr, ret,
                     (ret == ERRCODE_SUCC) ? "ACK!" : "no ACK");
    }
    osal_printk("=== Scan done ===\r\n\n");
    // ========== 扫描结束 ==========

    osal_printk("IIC完成\n");

    osal_task *taskid;
    osal_kthread_lock();

    taskid = osal_kthread_create((osal_kthread_handler)led_test_task, NULL, "led_test_task", 4096);
    (void)taskid; /* 创建后无需持有句柄，显式丢弃以消除 unused-but-set-variable 警告 */
    taskid = osal_kthread_create((osal_kthread_handler)cs43131_task, NULL, "cs43131_task", 4096);
    (void)taskid; /* 创建后无需持有句柄，显式丢弃以消除 unused-but-set-variable 警告 */

    osal_kthread_unlock();
}

app_run(app_entry);
