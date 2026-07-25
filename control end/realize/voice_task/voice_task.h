#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 模块固件中配置为“被动播报”的普通播报语 ID（逻辑帧 AA 55 FF ID FB）。 */
typedef enum {
    VOICE_PROMPT_MODE_SELECTION = 0x1A,
    VOICE_PROMPT_VOLUME_ADJUST = 0x1B,
    VOICE_PROMPT_BRIGHTNESS_ADJUST = 0x1C,
    VOICE_PROMPT_NETWORK_ON = 0x1D,
    VOICE_PROMPT_NETWORK_OFF = 0x1E,
    VOICE_PROMPT_HOTSPOT_ON = 0x64,
    VOICE_PROMPT_HOTSPOT_OFF = 0x65,
} voice_prompt_t;

void *voice_task(void *arg);

/**
 * 将一条被动播报请求放入语音任务队列。
 * 返回 true 表示请求已入队；实际 I2C 发送结果通过 [VOICE] 日志报告。
 */
bool voice_request_prompt(voice_prompt_t prompt);

bool voice_request_mode_selection_prompt(void);
bool voice_request_volume_adjust_prompt(void);
bool voice_request_brightness_adjust_prompt(void);
bool voice_request_network_on_prompt(void);
bool voice_request_network_off_prompt(void);
bool voice_request_hotspot_on_prompt(void);
bool voice_request_hotspot_off_prompt(void);

#ifdef __cplusplus
}
#endif
