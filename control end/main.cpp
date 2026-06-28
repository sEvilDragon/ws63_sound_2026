#include "app_init.h"
#include "soc_osal.h"
#include "nv.h"
#include "led_test.h"
#include "spi_task.h"
#include "ttp_task.h"
#include "ui_task.h"
#include "sk9822_task.h"

void app_entry(void)
{
    osal_task *taskid;

#if defined(CONFIG_MIDDLEWARE_SUPPORT_NV)
    /* uapi_nv_init() 已在系统初始化中调用, 此处只需加载配置。
     * 必须在 osal_kthread_lock() 之前调用，因为 NV 读操作内部
     * 使用信号量 (osal_sem_down_timeout)，依赖调度器运行。 */
    nv_load_settings();
#else
    osal_printk("[CTRL] NV NOT ENABLED in build config!\r\n");
#endif

    osal_kthread_lock();

    // taskid = osal_kthread_create((osal_kthread_handler)spi_slave_task, NULL, "spi_slave_task", 2048);
    // (void)taskid;

    taskid = osal_kthread_create((osal_kthread_handler)ttp_task, NULL, "ttp_task", 2048);
    (void)taskid;

    taskid = osal_kthread_create((osal_kthread_handler)ui_task, NULL, "ui_task", 2048);
    (void)taskid;

    taskid = osal_kthread_create((osal_kthread_handler)led_test_task, NULL, "led_test_task", 4096);
    (void)taskid;

    taskid = osal_kthread_create((osal_kthread_handler)sk9822_task, NULL, "sk9822_task", 4096*2);
    (void)taskid;

    // taskid = osal_kthread_create((osal_kthread_handler)cs43131_task, NULL, "cs43131_task", 4096);
    // (void)taskid;

    osal_kthread_unlock();
}

app_run(app_entry);
