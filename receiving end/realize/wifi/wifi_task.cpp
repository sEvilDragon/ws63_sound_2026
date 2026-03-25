#include "wifi_task.hpp"

void *wifi_task(void *arg)
{
    unused(arg);

    wifi wifi_;
    dlan dlan_;

    osal_printk("wifi任务启动，等待网络就绪后启动dlan\n");
    while (true) {
        if (wifi_.is_ready) {
            dlan_.is_ready_set(true);
            osal_printk("wifi已就绪，启动dlan扫描\n");
            break;
        }
        osal_msleep(1000);
    }

    dlan_.ssdp_and_http_scan();
    return NULL;
}