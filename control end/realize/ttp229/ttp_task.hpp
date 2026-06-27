#pragma once

extern "C" {
#include "pinctrl.h"
#include "gpio.h"
#include "soc_osal.h"
#include "app_init.h"
}

#include "ttp_task.h"

namespace sed_ws63 {

class ttp229 {
public:
    static constexpr uint32_t POLL_INTERVAL_MS = 5;
    static constexpr pin_t SCL_PIN = GPIO_12;
    static constexpr pin_t SDA_PIN = GPIO_00;

    // 当前芯片配置 (根据模块 TP 跳线决定, 若有变化请修改下列标志)
    //  - TP2 接 GND → 16 键模式已启用
    //  - TP0 默认悬空 → 输出正相 (按下=1); 若 TP0 也接了 GND, 改为 true
    //  - TP1 默认悬空 → 多键同时有效 (滑条质心算法需要)
    static constexpr bool ACTIVE_LOW = false;

    ttp229();
    ~ttp229() = default;

    bool read_touch(uint16_t *state_out);

    const ttp_state_t &get_state() const
    {
        return m_state;
    }
    bool is_healthy() const
    {
        return m_healthy;
    }

    void update();

private:
    // 滑条: 物理从左到右顺序 (chip pin 号, 1-based)
    // raw bit:        9  11   8  10  14  15   0   1
    // chip pin:      10  12   9  11  15  16   1   2
    // 如果发现滑动方向反了, 把整个数组 reverse 即可
    static constexpr int PAD_SLIDER[8] = {10, 12, 9, 11, 15, 16, 1, 2};
    // 功能键: raw bit 4/5/6 → chip pin 5/6/7
    static constexpr int PAD_FUNC_A = 5;
    static constexpr int PAD_FUNC_B = 6;
    static constexpr int PAD_FUNC_C = 7;

    static constexpr int SLIDER_HISTORY_SIZE = 8;
    static constexpr uint32_t SPEED_DECAY_TICKS = 500;
    static constexpr int FUNC_DEBOUNCE_TICKS = 2;
    // 状态翻转后短暂忽略反向抖动 (~100ms @33ms 扫描周期)
    static constexpr int FUNC_HOLD_TICKS = 3;

    bool m_healthy;
    ttp_state_t m_state;

    int m_history[SLIDER_HISTORY_SIZE];
    int m_history_idx;
    int m_history_count;

    int m_last_pos;
    uint32_t m_ticks_since_change;
    bool m_was_active;

    int m_func_debounce[TTP_FUNC_PADS_COUNT];
    int m_func_hold[TTP_FUNC_PADS_COUNT];
    bool m_func_stable[TTP_FUNC_PADS_COUNT];
    bool m_func_prev_raw[TTP_FUNC_PADS_COUNT];

    bool read_serial(uint16_t *state_out);
    void init_serial();

    void process_slider(uint16_t raw);
    void process_function_keys(uint16_t raw);
    int compute_slider_position(uint16_t raw);
    static bool is_pad_touched(uint16_t raw, int pad_number);
};

} // namespace sed_ws63
