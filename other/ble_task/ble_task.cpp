#include "ble_task.hpp"

void ble_task(void *arg)
{
    unused(arg);

    osal_printk("[BT] ble_task starting...\n");

    // 创建 bt 实例 — 构造函数自动完成所有初始化：
    //   1. gap_register_callbacks()
    //   2. avrcp_tg_register_callbacks()
    //   3. enable_bt_stack()
    // 当 BT stack TURN_ON 时回调中自动：
    //   4. 设置本地设备名称/地址
    //   5. 注册 A2DP 音频监听器（启动 Sink 角色）
    //   6. 注册 AVRCP Target 媒体按键
    //   7. 开启可发现模式
    static bt bt_instance;

    // 可选：如果硬件直通路径 (share_mem_id=0) 不工作，
    // 改为注册手动数据回调走共享内存路径：
    // bt::set_data_process_function(iis::data_write);
    // bt::set_data_clear_function(iis::data_clear);

    osal_printk("[BT] bt_instance created, waiting for connections...\n");

    // 蓝牙完全是事件驱动的，任务主体只需保持存活
    while (true) {
        osal_msleep(1000);
    }
}
