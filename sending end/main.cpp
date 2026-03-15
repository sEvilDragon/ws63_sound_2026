#include "app_init.h"
#include "soc_osal.h"
#include "led_test.h"
#include "audio_read_send.h"

void app_entry(void)
{
    osal_task *taskid;
    osal_kthread_lock();

    taskid = osal_kthread_create((osal_kthread_handler)audio_read_send_task, NULL, "audio_read_send_task", 8192);
    if (taskid == nullptr) {
        osal_kthread_set_priority(taskid, 26);
        osal_printk("音频读取发送任务创建失败\n");
    }

    taskid = osal_kthread_create((osal_kthread_handler)led_test_task, NULL, "led_test_task", 4096);
    (void)taskid; /* 创建后无需持有句柄，显式丢弃以消除 unused-but-set-variable 警告 */

    osal_kthread_unlock();
}

app_run(app_entry);