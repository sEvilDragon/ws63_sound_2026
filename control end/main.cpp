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
    osal_task *taskid;
    osal_kthread_lock();

    // taskid = osal_kthread_create((osal_kthread_handler)led_test_task, NULL, "led_test_task", 4096);
    // (void)taskid; /* 创建后无需持有句柄，显式丢弃以消除 unused-but-set-variable 警告 */
    taskid = osal_kthread_create((osal_kthread_handler)cs43131_task, NULL, "cs43131_task", 4096);
    (void)taskid; /* 创建后无需持有句柄，显式丢弃以消除 unused-but-set-variable 警告 */

    osal_kthread_unlock();
}

app_run(app_entry);
