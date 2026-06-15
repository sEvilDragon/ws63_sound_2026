#include "app_init.h"
#include "soc_osal.h"
#include "led_test.h"
#include "spi_slave.hpp"

static void *spi_slave_task(void *arg)
{
    (void)arg;
    static sed_ws63::spi_slave g_spi_slave;
    osal_printk("[SPI_Slave] 任务启动\r\n");

    uint8_t rx_buf[sed_ws63::spi_slave::TRANSFER_LEN];
    const char *ack = "ok";

    while (true) {
        // 等待主机数据并回复(固定 16 字节, 参照官方 demo)
        int ret = g_spi_slave.transfer(rx_buf, sizeof(rx_buf), (const uint8_t *)ack, 2);
        if (ret == 0) {
            osal_printk("[SPI_Slave] 收到: ");
            for (uint32_t i = 0; i < sed_ws63::spi_slave::TRANSFER_LEN; i++) {
                if (rx_buf[i] >= 0x20 && rx_buf[i] < 0x7F) {
                    osal_printk("%c", rx_buf[i]);
                }
            }
            osal_printk("\r\n");
        }
        osal_msleep(500); // 与 Master 同步周期(参照官方 demo)
    }
    return nullptr;
}

void app_entry(void)
{
    osal_task *taskid;
    osal_kthread_lock();

    // 创建 SPI Slave 通讯任务
    taskid = osal_kthread_create((osal_kthread_handler)spi_slave_task, NULL, "spi_slave_task", 2048);
    (void)taskid;

    // taskid = osal_kthread_create((osal_kthread_handler)led_test_task, NULL, "led_test_task", 4096);
    // (void)taskid; /* 创建后无需持有句柄，显式丢弃以消除 unused-but-set-variable 警告 */
    // taskid = osal_kthread_create((osal_kthread_handler)cs43131_task, NULL, "cs43131_task", 4096);
    // (void)taskid; /* 创建后无需持有句柄，显式丢弃以消除 unused-but-set-variable 警告 */

    osal_kthread_unlock();
}

app_run(app_entry);
