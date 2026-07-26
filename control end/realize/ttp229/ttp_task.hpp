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
    static constexpr bool ACTIVE_LOW = true;

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
    // 全部 16 个触点都不再区分位置，任意触点按下都视为同一个逻辑按键。
    static constexpr uint16_t BUTTON_RAW_MASK = 0xFFFF;
    static constexpr int BUTTON_PRESS_DEBOUNCE_TICKS = 2;
    static constexpr int BUTTON_RELEASE_DEBOUNCE_TICKS = 3;

    bool m_healthy;
    ttp_state_t m_state;
    int m_button_debounce;
    bool m_button_stable;

    bool read_serial(uint16_t *state_out);
    void init_serial();

    void process_button(uint16_t raw);
};

} // namespace sed_ws63
