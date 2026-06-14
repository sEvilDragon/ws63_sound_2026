#include "app_init.h"
#include "soc_osal.h"
#include "led_test.h"
#include "audio_play.hpp"
#include "wifi_task.hpp"
#include "spi_master.hpp"

static void *spi_master_task(void *arg)
{
    (void)arg;
    static sed_ws63::spi_master g_spi_master;
    osal_printk("[SPI_Master] 任务启动, 开始轮询通讯...\r\n");

    uint8_t rx_buf[255];
    while (true) {
        // 示例: 发送数据并接收响应
        const char *msg = "hello";
        int rx_len = g_spi_master.writeread((const uint8_t *)msg, 5, rx_buf, sizeof(rx_buf));
        if (rx_len > 0) {
            osal_printk("[SPI_Master] 收到 %d 字节响应\r\n", rx_len);
        }

        osal_msleep(500); // 500ms 轮询间隔
    }
    return nullptr;
}

void app_entry(void)
{
    osal_task *taskid;
    osal_kthread_lock();

    taskid = osal_kthread_create((osal_kthread_handler)led_test_task, NULL, "led_test_task", 1024);
    (void)taskid; /* 创建后无需持有句柄，显式丢弃以消除 unused-but-set-variable 警告 */

    // 创建 SPI Master 通讯任务
    taskid = osal_kthread_create((osal_kthread_handler)spi_master_task, NULL, "spi_master_task", 2048);
    (void)taskid;

    // taskid = osal_kthread_create((osal_kthread_handler)audio_play_task, NULL, "audio_play_task", 4096);
    // (void)taskid; /* 创建后无需持有句柄，显式丢弃以消除 unused-but-set-variable 警告 */

    // taskid = osal_kthread_create((osal_kthread_handler)wifi_task, NULL, "wifi_task", 8192);
    // (void)taskid; /* 创建后无需持有句柄，显式丢弃以消除 unused-but-set-variable 警告 */

    // taskid = osal_kthread_create((osal_kthread_handler)minimp3_task, NULL, "minimp3_task", 8192 * 4);
    // (void)taskid;

    // // 创建ble任务
    // taskid = osal_kthread_create((osal_kthread_handler)ble_task, NULL, "ble_task", 4096);
    // (void)taskid;

    osal_kthread_unlock();
}

app_run(app_entry);