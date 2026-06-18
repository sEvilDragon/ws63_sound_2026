#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TTP_TOTAL_PADS 16
#define TTP_SLIDER_PADS_COUNT 8
#define TTP_FUNC_PADS_COUNT 3

// 滑条位置缩放因子: 内部用 ×100 精度, 0..700 对应逻辑档位 0.00..7.00
// 质心算法在此精度下可提供亚位插值, 实现流畅的跟手滑动
#define TTP_SLIDER_SCALE 100

#define TTP_SWIPE_NONE 0
#define TTP_SWIPE_LEFT 1
#define TTP_SWIPE_RIGHT 2

#define TTP_SLIDER_NO_POS (-1)

#define TTP_FUNC_IDX_A 0
#define TTP_FUNC_IDX_B 1
#define TTP_FUNC_IDX_C 2

typedef struct {
    uint16_t raw_state;

    int slider_pos;
    int slider_speed;
    uint8_t slider_direction;

    uint8_t func_pressed;
    uint8_t func_just_pressed;
    uint8_t func_just_released;
} ttp_state_t;

// 边沿锁存 API: ttp_task 每帧把新产生的边沿 OR 进 latch,
// UI 侧用 ttp_consume_*_latch() 原子读出并清零, 保证每个事件只被处理一次,
// 彻底消除双任务 5ms/33ms 轮询速率差造成的重复触发.
uint8_t ttp_consume_press_latch(void);
uint8_t ttp_consume_release_latch(void);

static inline int ttp_func_is_pressed(const ttp_state_t *s, int idx)
{
    return (s->func_pressed >> idx) & 1;
}
static inline int ttp_func_is_just_pressed(const ttp_state_t *s, int idx)
{
    return (s->func_just_pressed >> idx) & 1;
}
static inline int ttp_func_is_just_released(const ttp_state_t *s, int idx)
{
    return (s->func_just_released >> idx) & 1;
}

void *ttp_task(void *arg);

const ttp_state_t *ttp_get_state(void);

#ifdef __cplusplus
}
#endif
