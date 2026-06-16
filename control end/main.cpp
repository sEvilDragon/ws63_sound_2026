#include "app_init.h"
#include "soc_osal.h"
#include "led_test.h"
#include "spi_task.h"

void app_entry(void)
{
    osal_task *taskid;
    osal_kthread_lock();

    taskid = osal_kthread_create((osal_kthread_handler)spi_slave_task, NULL, "spi_slave_task", 2048);
    (void)taskid;

    taskid = osal_kthread_create((osal_kthread_handler)led_test_task, NULL, "led_test_task", 4096);
    (void)taskid;
    // taskid = osal_kthread_create((osal_kthread_handler)cs43131_task, NULL, "cs43131_task", 4096);
    // (void)taskid;

    osal_kthread_unlock();
}

app_run(app_entry);
