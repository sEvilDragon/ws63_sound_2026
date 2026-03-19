#include "wifi_task.hpp"

void *wifi_task(void *arg)
{
    unused(arg);

    wifi wifi_;
    while (true) {
        osal_msleep(1000);
    }
    return NULL;
}