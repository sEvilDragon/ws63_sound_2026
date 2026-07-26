#ifndef __SK9822_TASK_H__
#define __SK9822_TASK_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *sk9822_task(void *arg);

// target: 0=全灭(模式), 1=1/3(音量), 2=2/3(网络), 3=全绿(热点)
void sk9822_show_control_target(uint8_t target);

#ifdef __cplusplus
}
#endif

#endif
