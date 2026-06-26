#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "soc_osal.h"

#ifdef __cplusplus
}
#endif

// HTTP 控制服务器任务入口（由 wifi_task 动态创建/销毁）
void *http_control_task(void *arg);

// 请求停止 HTTP 控制服务器（设置标志位，任务在 accept 超时后退出）
void http_control_request_stop(void);

// 重置停止标志（启动前调用）
void http_control_reset_stop(void);
