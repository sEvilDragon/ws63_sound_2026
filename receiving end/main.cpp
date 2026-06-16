#include "app_init.h"
#include "soc_osal.h"
#include "led_test.h"
#include "audio_play.hpp"
#include "wifi_task.hpp"
#include "spi_task.h"

void app_entry(void)
{
    osal_task *taskid;
    osal_kthread_lock();

    taskid = osal_kthread_create((osal_kthread_handler)led_test_task, NULL, "led_test_task", 1024);
    (void)taskid;

    taskid = osal_kthread_create((osal_kthread_handler)spi_master_task, NULL, "spi_master_task", 2048);
    (void)taskid;

    // taskid = osal_kthread_create((osal_kthread_handler)audio_play_task, NULL, "audio_play_task", 4096);
    // (void)taskid;

    // taskid = osal_kthread_create((osal_kthread_handler)wifi_task, NULL, "wifi_task", 8192);
    // (void)taskid;

    // taskid = osal_kthread_create((osal_kthread_handler)minimp3_task, NULL, "minimp3_task", 8192 * 4);
    // (void)taskid;

    // taskid = osal_kthread_create((osal_kthread_handler)ble_task, NULL, "ble_task", 4096);
    // (void)taskid;

    osal_kthread_unlock();
}

app_run(app_entry);
