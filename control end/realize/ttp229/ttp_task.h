#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TTP_TOTAL_PADS 16

typedef struct {
    uint16_t raw_state;
    uint8_t button_pressed;
    uint8_t button_just_pressed;
    uint8_t button_just_released;
} ttp_state_t;

void *ttp_task(void *arg);

const ttp_state_t *ttp_get_state(void);

#ifdef __cplusplus
}
#endif
