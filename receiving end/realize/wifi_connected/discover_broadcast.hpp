#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "soc_osal.h"

#ifdef __cplusplus
}
#endif

// UDP 发现广播任务入口（由 wifi_task 在 STA 连接后创建，断开后销毁）
void *discover_broadcast_task(void *arg);

// 请求停止广播（设置标志位，任务在 sleep 超时后退出）
void discover_broadcast_request_stop(void);

// 重置停止标志（启动前调用）
void discover_broadcast_reset_stop(void);
