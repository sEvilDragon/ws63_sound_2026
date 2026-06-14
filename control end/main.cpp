#include "app_init.h"
#include "soc_osal.h"
#include "led_test.h"
#include "spi_slave.hpp"

static void *spi_slave_task(void *arg)
{
    (void)arg;
    static sed_ws63::spi_slave g_spi_slave;
    osal_printk("[SPI_Slave] 任务启动, 等待主机通讯...\r\n");

    uint8_t rx_buf[255];
    while (true) {
        // 从模式: 等待主机发起传输
        int rx_len = g_spi_slave.recv(rx_buf, sizeof(rx_buf));
        if (rx_len > 0) {
            osal_printk("[SPI_Slave] 收到 %d 字节数据\r\n", rx_len);

            // 回复确认
            const char *ack = "ok";
            g_spi_slave.send((const uint8_t *)ack, 2);
        }

        osal_msleep(100); // 100ms 轮询间隔(从模式需频繁检查)
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
