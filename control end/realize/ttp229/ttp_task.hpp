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
    // 滑条: 物理从左到右 (10→2, 印 0→7)
    static constexpr int PAD_SLIDER[8] = {10, 12, 9, 11, 15, 16, 1, 2};
    // Function keys from measured C/B/A sequence: A=Pad7, B=Pad6, C=Pad5
    static constexpr int PAD_FUNC_A = 7;
    static constexpr int PAD_FUNC_B = 6;
    static constexpr int PAD_FUNC_C = 5;

    static constexpr int FUNC_DEBOUNCE_TICKS = 2;
    // 状态翻转后短暂忽略反向抖动 (~195ms @39ms) 防止释放后立即误触发
    static constexpr int FUNC_HOLD_TICKS = 5;

    bool m_healthy;
    ttp_state_t m_state;

    // 运动追踪: 比较相邻帧 pad 集合变化 (进入/离开) 判断滑动方向
    // 积分器累积方向证据, 超过阈值发射一步, 自然过滤对称振荡
    static constexpr int SLIDER_INTEGRATOR_MAX = 6 * TTP_SLIDER_SCALE; // 积分器饱和值
    static constexpr int SLIDER_FIRE_THRESHOLD = 2 * TTP_SLIDER_SCALE; // 发射阈值 = 2 pad 宽度, 降速
    static constexpr int SLIDER_STEP = TTP_SLIDER_SCALE;               // 每步 = 1 逻辑档位, 降速
    uint8_t m_slider_prev;                                             // 上一帧 uint8_t 滑条状态
    int m_slider_integrator;                                           // 方向积分器: >0 右, <0 左
    int m_slider_last_lock;                                            // 方向锁定: -1=锁定右滑, +1=锁定左滑, 0=未锁定
                                                                       // 防止边界抖动造成正负交替误触发
    int m_slider_notouch_cnt;                                          // 连续无触摸帧计数, 用于抬手检测

    int m_func_debounce[TTP_FUNC_PADS_COUNT];
    int m_func_hold[TTP_FUNC_PADS_COUNT];
    bool m_func_stable[TTP_FUNC_PADS_COUNT];
    bool m_func_prev_raw[TTP_FUNC_PADS_COUNT];

    bool read_serial(uint16_t *state_out);
    void init_serial();

    void process_slider(uint16_t raw);
    void process_function_keys(uint16_t raw);
    static bool is_pad_touched(uint16_t raw, int pad_number);
};

} // namespace sed_ws63
