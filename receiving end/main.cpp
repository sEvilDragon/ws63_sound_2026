#include "app_init.h"
#include "soc_osal.h"
#include "nv.h"
#include "led_test.h"
#include "audio_play.hpp"
#include "wifi_task.hpp"
#include "spi_task.h"
#include "nv_recv.hpp"

#include "dma.h"

// 裸机系统无 atexit 支持，提供空桩函数
// 函数内 static 局部变量（如 iis、sta）的析构函数注册需要此符号
extern "C" int atexit(void (*func)(void))
{
    (void)func;
    return 0;
}

void app_entry(void)
{
    osal_task *taskid;

    /* DMA 必须只初始化一次, 避免 hal_dma_v151_open 重置通道状态 */
    uapi_dma_init();
    uapi_dma_open();

#if defined(CONFIG_MIDDLEWARE_SUPPORT_NV)
    /* uapi_nv_init() 已在系统初始化中调用, 此处只需加载配置。
     * 必须在 osal_kthread_lock() 之前调用，因为 NV 操作内部
     * 使用信号量，依赖调度器运行。 */
    nv_recv_load_all();
    bool spi_settings_loaded = (spi_settings_load_from_nv() != 0);
#else
    osal_printk("[RECV] NV NOT ENABLED in build config!\r\n");
    bool spi_settings_loaded = false;
#endif

    osal_kthread_lock();

    if (!spi_settings_loaded) {
        spi_settings_update_mode(SPI_MODE_SLE);
    }

    taskid = osal_kthread_create((osal_kthread_handler)led_test_task, NULL, "led_test_task", 1024);
    osal_printk("[RECV] led_test_task created: %p\r\n", taskid);
    (void)taskid;

    taskid = osal_kthread_create((osal_kthread_handler)spi_master_task, NULL, "spi_master_task", 4096);
    osal_printk("[RECV] spi_master_task created: %p\r\n", taskid);
    (void)taskid;

    taskid = osal_kthread_create((osal_kthread_handler)audio_play_task, NULL, "audio_play_task", 4096);
    (void)taskid;

    taskid = osal_kthread_create((osal_kthread_handler)wifi_task, NULL, "wifi_task", 4096 * 3);
    (void)taskid;

    // minimp3_task is created dynamically by wifi_task when DLNA mode starts
    // taskid = osal_kthread_create((osal_kthread_handler)minimp3_task, NULL, "minimp3_task", 8192 * 4);
    // (void)taskid;

    // taskid = osal_kthread_create((osal_kthread_handler)ble_task, NULL, "ble_task", 4096);
    // (void)taskid;

    osal_kthread_unlock();
}

app_run(app_entry);
