#include "app_init.h"
#include "soc_osal.h"
#include "led_test.h"
#include "audio_play.hpp"
#include "wifi_task.hpp"
#include "spi_task.h"

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
    osal_kthread_lock();

    spi_settings_update_mode(SPI_MODE_SLE);

    taskid = osal_kthread_create((osal_kthread_handler)led_test_task, NULL, "led_test_task", 1024);
    (void)taskid;

    // taskid = osal_kthread_create((osal_kthread_handler)spi_master_task, NULL, "spi_master_task", 2048);
    // (void)taskid;

    taskid = osal_kthread_create((osal_kthread_handler)audio_play_task, NULL, "audio_play_task", 3072);
    (void)taskid;

    taskid = osal_kthread_create((osal_kthread_handler)wifi_task, NULL, "wifi_task", 6144);
    (void)taskid;

    // minimp3_task is created dynamically by wifi_task when DLNA mode starts
    // taskid = osal_kthread_create((osal_kthread_handler)minimp3_task, NULL, "minimp3_task", 8192 * 4);
    // (void)taskid;

    // taskid = osal_kthread_create((osal_kthread_handler)ble_task, NULL, "ble_task", 4096);
    // (void)taskid;

    osal_kthread_unlock();
}

app_run(app_entry);
