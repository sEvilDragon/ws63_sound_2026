#include "ttp_task.hpp"

extern "C" {
#include "gpio.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "app_init.h"
}

// 详细诊断开关: 1 = 输出逐帧原始值/按键消抖/质心细节; 0 = 仅关键事件
// 默认 0, 调试 bit-bang 信号/消抖细节时手动改为 1 重新编译
#ifndef TTP_VERBOSE
#define TTP_VERBOSE 0
#endif

#define TTP_LOG(fmt, ...) osal_printk("[TTP229] " fmt, ##__VA_ARGS__)
#if TTP_VERBOSE
#define TTP_VLOG(fmt, ...) osal_printk("[TTP229][v] " fmt, ##__VA_ARGS__)
#else
#define TTP_VLOG(fmt, ...) \
    do {                   \
        (void)0;           \
    } while (0)
#endif

namespace sed_ws63 {

ttp229::ttp229()
    : m_healthy(false),
      m_state{},
      m_history{},
      m_history_idx(0),
      m_history_count(0),
      m_last_pos(TTP_SLIDER_NO_POS),
      m_ticks_since_change(0),
      m_was_active(false),
      m_func_debounce{},
      m_func_hold{},
      m_func_stable{},
      m_func_prev_raw{}
{
    init_serial();
}

void ttp229::init_serial()
{
    TTP_LOG("init: SCL=GPIO%d, SDO=GPIO%d, ACTIVE_LOW=%d\r\n", (int)SCL_PIN, (int)SDA_PIN, ACTIVE_LOW ? 1 : 0);

    uapi_pin_set_mode(SCL_PIN, PIN_MODE_0);
    uapi_pin_set_mode(SDA_PIN, PIN_MODE_0);
    uapi_gpio_set_dir(SCL_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_dir(SDA_PIN, GPIO_DIRECTION_INPUT);
    uapi_gpio_set_val(SCL_PIN, GPIO_LEVEL_HIGH);

    osal_msleep(2);

    // 探测: 在拉低前先看 SDO 当前电平 (悬空/上拉应为 1; 若为 0 说明线接反或短路)
    uint32_t sdo_before = uapi_gpio_get_val(SDA_PIN);
    uapi_gpio_set_val(SCL_PIN, GPIO_LEVEL_LOW);
    uint32_t sdo_after = uapi_gpio_get_val(SDA_PIN);
    TTP_LOG("init: SDO before_clk=%u after_clk_low=%u (both=1 正常, both=0 可能悬空)\r\n", (unsigned)sdo_before,
            (unsigned)sdo_after);

    uapi_gpio_set_val(SCL_PIN, GPIO_LEVEL_HIGH);

    uint16_t sample = 0;
    if (read_serial(&sample)) {
        m_healthy = true;
        TTP_LOG("serial probe OK, raw=0x%04x (touch a pad to verify)\r\n", sample);
        // 多读几次, 帮助确认线路是否稳定
        uint16_t s2 = 0, s3 = 0;
        read_serial(&s2);
        read_serial(&s3);
        TTP_LOG("serial probe x3: 0x%04x / 0x%04x / 0x%04x\r\n", sample, s2, s3);
    } else {
        TTP_LOG("serial probe FAILED\r\n");
    }
}

bool ttp229::read_serial(uint16_t *state_out)
{
    if (!state_out)
        return false;

    // TTP229-BSF 帧起始: SCL HIGH ≥ 2ms (这里由上一次末尾的 HIGH 保证),
    // 然后拉 LOW ≥ 1ms 再开始逐位采样.
    uapi_gpio_set_val(SCL_PIN, GPIO_LEVEL_LOW);
    osal_udelay(1100);

    uint16_t result = 0;
#if TTP_VERBOSE
    char bits[17];
    bits[16] = '\0';
#endif
    for (int i = 0; i < 16; i++) {
        // SCL HIGH ≥ 1ms, 在中段读取 SDO
        uapi_gpio_set_val(SCL_PIN, GPIO_LEVEL_HIGH);
        osal_udelay(1000);

        uint8_t bit = (uint8_t)(uapi_gpio_get_val(SDA_PIN) & 0x01);
        result = (uint16_t)((result << 1) | bit);
#if TTP_VERBOSE
        bits[i] = bit ? '1' : '0';
#endif

        // SCL LOW ≥ 1ms, 芯片在此阶段准备下一位
        uapi_gpio_set_val(SCL_PIN, GPIO_LEVEL_LOW);
        osal_udelay(1000);
    }

    uapi_gpio_set_val(SCL_PIN, GPIO_LEVEL_HIGH);
    *state_out = result;
#if TTP_VERBOSE
    {
        static uint16_t last_rs = 0xFFFF;
        static uint32_t rs_calls = 0;
        rs_calls++;
        if (result != last_rs || rs_calls <= 5) {
            TTP_VLOG("read_serial#%u: bits=%s raw=0x%04x (prev=0x%04x)\r\n", (unsigned)rs_calls, bits, result, last_rs);
            last_rs = result;
        }
    }
#endif
    return true;
}

bool ttp229::read_touch(uint16_t *state_out)
{
    if (!state_out)
        return false;

    uint16_t serial_raw = 0;
    if (!read_serial(&serial_raw))
        return false;

    // 芯片 MSB-first 输出, 第 N 个时钟采到的 bit 对应 chip pin (16-N).
    // 累积后: serial_raw bit N 对应 chip pin (N+1), 1-based.
    // 旧代码做过一次 16-bit 反转, 结果整体错位一位, 导致:
    //   - pad 1 (bit 0) 被当成 bit 1, 完全没响应
    //   - pad 16 永远读不到 (bit 15 被反转到 bit 0)
    //   - 功能键整体错位一档 (pad 14 报成 pad 13)
    // 现在直接用 serial_raw, 配合 is_pad_touched 的 (pad-1) 位检测.
    uint16_t out = serial_raw;
    if (ACTIVE_LOW) {
        out = (uint16_t)(~out);
    }
#if TTP_VERBOSE
    {
        static uint16_t last_rt = 0xFFFF;
        if (out != last_rt) {
            TTP_VLOG("read_touch: serial=0x%04x -> out=0x%04x (ACTIVE_LOW=%d)\r\n", serial_raw, out,
                     ACTIVE_LOW ? 1 : 0);
            last_rt = out;
        }
    }
#endif
    *state_out = out;
    return true;
}

bool ttp229::is_pad_touched(uint16_t raw, int pad_number)
{
    // pad_number 是 1-based 芯片引脚号 (1..16), raw bit N = chip pin (N+1)
    if (pad_number < 1 || pad_number > TTP_TOTAL_PADS)
        return false;
    return (raw & (1u << (pad_number - 1))) != 0;
}

int ttp229::compute_slider_position(uint16_t raw)
{
    // 最大连续段算法: 找到 PAD_SLIDER 索引中连续被按下的最长一段,
    // 取其质心。非连续位置的孤立触发（如手指在右边但左边 pad 也被意外桥接）
    // 会被自动忽略, 不会把质心拖偏。
    //
    // 平局规则: 优先选离上次位置 (m_last_pos) 最近的段;
    // 若无上次位置, 选最右段 (手指更可能在右边)。
    bool active[TTP_SLIDER_PADS_COUNT];
    for (int i = 0; i < TTP_SLIDER_PADS_COUNT; i++) {
        active[i] = is_pad_touched(raw, PAD_SLIDER[i]);
    }

    int best_start = -1;
    int best_len = 0;
    int run_start = -1;
    int run_len = 0;

    auto pick_best = [&](int start, int len) {
        if (len > best_len) {
            best_start = start;
            best_len = len;
        } else if (len == best_len && len > 0) {
            // 平局: 优先选离上次位置更近的段
            int prev_idx = (m_last_pos >= 0) ? m_last_pos / TTP_SLIDER_SCALE : -1;
            if (prev_idx >= 0) {
                int dist_new = (start + len / 2) - prev_idx;
                int dist_best = (best_start + best_len / 2) - prev_idx;
                if (dist_new < 0)
                    dist_new = -dist_new;
                if (dist_best < 0)
                    dist_best = -dist_best;
                if (dist_new < dist_best) {
                    best_start = start;
                    best_len = len;
                }
            } else {
                // 无历史位置, 选最右 (手指更可能在右边)
                if (start > best_start) {
                    best_start = start;
                    best_len = len;
                }
            }
        }
    };

    for (int i = 0; i < TTP_SLIDER_PADS_COUNT; i++) {
        if (active[i]) {
            if (run_start < 0)
                run_start = i;
            run_len++;
        } else {
            pick_best(run_start, run_len);
            run_start = -1;
            run_len = 0;
        }
    }
    pick_best(run_start, run_len); // 末尾段

    if (best_len == 0)
        return TTP_SLIDER_NO_POS;

    // 最大连续段质心
    int sum_index = 0;
    for (int i = best_start; i < best_start + best_len; i++) {
        sum_index += i;
    }
    return (sum_index * TTP_SLIDER_SCALE) / best_len;
}

void ttp229::process_slider(uint16_t raw)
{
    int prev_pos = m_state.slider_pos;
    int pos = compute_slider_position(raw);
    m_state.slider_pos = pos;

    bool active = (pos != TTP_SLIDER_NO_POS);

    if (!active) {
        if (prev_pos != TTP_SLIDER_NO_POS) {
            TTP_LOG("slider: released (was pos=%d.%02d)\r\n", prev_pos / TTP_SLIDER_SCALE, prev_pos % TTP_SLIDER_SCALE);
        }
        m_history_count = 0;
        m_history_idx = 0;
        m_ticks_since_change = 0;
        m_was_active = false;
        m_last_pos = TTP_SLIDER_NO_POS; // 抬手清零, 避免下次段选择被旧位置误导
        m_state.slider_direction = TTP_SWIPE_NONE;
        m_state.slider_speed = 0;
        return;
    }

    if (prev_pos != pos) {
        TTP_LOG("slider: pos=%d.%02d (raw=0x%04x, prev=%d.%02d)\r\n", pos / TTP_SLIDER_SCALE, pos % TTP_SLIDER_SCALE,
                raw, prev_pos / TTP_SLIDER_SCALE, prev_pos % TTP_SLIDER_SCALE);
    }

    m_history[m_history_idx] = pos;
    m_history_idx = (m_history_idx + 1) % SLIDER_HISTORY_SIZE;
    if (m_history_count < SLIDER_HISTORY_SIZE)
        m_history_count++;

    if (!m_was_active) {
        m_was_active = true;
        m_last_pos = pos;
        m_ticks_since_change = 0;
        m_state.slider_direction = TTP_SWIPE_NONE;
        m_state.slider_speed = 0;
        return;
    }

    m_ticks_since_change++;

    if (pos != m_last_pos) {
        int delta = pos - m_last_pos;

        // speed: 逻辑档位/秒, 归一化到缩放前的量级以保持 UI 阈值兼容
        int speed = 0;
        if (m_ticks_since_change > 0) {
            speed = (delta * 100) / ((int)m_ticks_since_change * TTP_SLIDER_SCALE);
        }

        uint8_t dir = TTP_SWIPE_NONE;
        if (delta < 0)
            dir = TTP_SWIPE_LEFT;
        else if (delta > 0)
            dir = TTP_SWIPE_RIGHT;

        m_state.slider_direction = dir;
        m_state.slider_speed = speed;

        TTP_VLOG("slider: move %d.%02d->%d.%02d delta=%d.%02d speed=%d dir=%u\r\n", m_last_pos / TTP_SLIDER_SCALE,
                 m_last_pos % TTP_SLIDER_SCALE, pos / TTP_SLIDER_SCALE, pos % TTP_SLIDER_SCALE,
                 delta / TTP_SLIDER_SCALE, delta < 0 ? ((-delta) % TTP_SLIDER_SCALE) : (delta % TTP_SLIDER_SCALE),
                 speed, (unsigned)dir);

        m_last_pos = pos;
        m_ticks_since_change = 0;
    } else if (m_ticks_since_change > SPEED_DECAY_TICKS) {
        int s = m_state.slider_speed;
        s = s >> 1;
        m_state.slider_speed = s;
        if (s == 0) {
            m_state.slider_direction = TTP_SWIPE_NONE;
        }
    }
}

void ttp229::process_function_keys(uint16_t raw)
{
    static const int pad_of_idx[TTP_FUNC_PADS_COUNT] = {PAD_FUNC_A, PAD_FUNC_B, PAD_FUNC_C};

    uint8_t pressed = 0;
    uint8_t just_p = 0;
    uint8_t just_r = 0;

    uint8_t prev_pressed = m_state.func_pressed;

    for (int i = 0; i < TTP_FUNC_PADS_COUNT; i++) {
        bool cur_raw = is_pad_touched(raw, pad_of_idx[i]);

        bool prev_raw = m_func_prev_raw[i];
        int prev_db = m_func_debounce[i];
        bool prev_stb = m_func_stable[i];
        int prev_hold = m_func_hold[i];

        // 最小保持窗口内: 强制忽略反向变化
        if (m_func_hold[i] > 0) {
            m_func_hold[i]--;
            m_func_debounce[i] = 0;
            m_func_prev_raw[i] = cur_raw;
        } else if (cur_raw == prev_stb) {
            // raw 与 stable 一致: 无边缘, 消抖计数器清零
            m_func_debounce[i] = 0;
        } else {
            // raw 与 stable 不一致: 累计一次, 达到阈值就接受新状态
            m_func_debounce[i]++;
            if (m_func_debounce[i] >= FUNC_DEBOUNCE_TICKS) {
                m_func_stable[i] = cur_raw;
                m_func_debounce[i] = 0;
                m_func_hold[i] = FUNC_HOLD_TICKS;
                TTP_VLOG("key[%d]=pad%d ACCEPT %d->%d\r\n", i, pad_of_idx[i], prev_stb ? 1 : 0, cur_raw ? 1 : 0);
            }
        }
        m_func_prev_raw[i] = cur_raw;

        bool stable = m_func_stable[i];
        if (stable)
            pressed |= (1u << i);

        bool prev_in_state = ((m_state.func_pressed >> i) & 1) != 0;
        if (stable && !prev_in_state)
            just_p |= (1u << i);
        if (!stable && prev_in_state)
            just_r |= (1u << i);

        // 仅在 raw/stable 发生变化时才输出, 避免 200Hz 刷屏
        if (cur_raw != prev_raw || stable != prev_stb || prev_db != m_func_debounce[i] || prev_hold != m_func_hold[i]) {
            TTP_VLOG("key[%d]=pad%d raw=%d->%d db=%d->%d stable=%d->%d hold=%d->%d\r\n", i, pad_of_idx[i],
                     prev_raw ? 1 : 0, cur_raw ? 1 : 0, prev_db, m_func_debounce[i], prev_stb ? 1 : 0, stable ? 1 : 0,
                     prev_hold, m_func_hold[i]);
        }
    }

    m_state.func_pressed = pressed;
    m_state.func_just_pressed = just_p;
    m_state.func_just_released = just_r;

    if (pressed != prev_pressed || just_p || just_r) {
        TTP_LOG("keys: pressed=0x%02x just_p=0x%02x just_r=0x%02x raw=0x%04x\r\n", pressed, just_p, just_r, raw);
    }
}

void ttp229::update()
{
    uint16_t raw = 0;
    if (!read_touch(&raw)) {
        return;
    }

    m_state.raw_state = raw;
    process_slider(raw);
    process_function_keys(raw);
}

} // namespace sed_ws63

static sed_ws63::ttp229 *g_ttp = nullptr;
static ttp_state_t g_ttp_state;

// 边沿锁存: ttp_task 累积, ui_task 读后清零
static volatile uint8_t g_press_latch = 0;
static volatile uint8_t g_release_latch = 0;

const ttp_state_t *ttp_get_state(void)
{
    return &g_ttp_state;
}

uint8_t ttp_consume_press_latch(void)
{
    uint8_t v = g_press_latch;
    g_press_latch = 0;
    return v;
}

uint8_t ttp_consume_release_latch(void)
{
    uint8_t v = g_release_latch;
    g_release_latch = 0;
    return v;
}

void *ttp_task(void *arg)
{
    (void)arg;

    static sed_ws63::ttp229 s_ttp;
    g_ttp = &s_ttp;

    TTP_LOG("task started (serial: SCL=GPIO%d, SDO=GPIO%d, 16-key, ACTIVE_LOW=%d)\r\n", (int)sed_ws63::ttp229::SCL_PIN,
            (int)sed_ws63::ttp229::SDA_PIN, sed_ws63::ttp229::ACTIVE_LOW ? 1 : 0);

    uint32_t ticks = 0;
    uint32_t frame_counter = 0;
    uint32_t dump_ticks = 0;
    uint32_t hb_ticks = 0;
    uint16_t last_raw = 0;
    uint8_t last_pressed = 0;

    while (true) {
        g_ttp->update();
        g_ttp_state = g_ttp->get_state();

        // 把本帧边沿累加进锁存, 保证 ui_task 无论何时读都能拿到, 且只消费一次
        if (g_ttp_state.func_just_pressed)
            g_press_latch |= g_ttp_state.func_just_pressed;
        if (g_ttp_state.func_just_released)
            g_release_latch |= g_ttp_state.func_just_released;

        frame_counter++;

        const ttp_state_t *s = &g_ttp_state;

        // 启动后前 5 帧: 强制打印原始值, 立刻判断 SDO 是否完全没信号
        if (frame_counter <= 5) {
            TTP_LOG("boot frame #%u: raw=0x%04x slider=%d.%02d fn=0x%02x\r\n", (unsigned)frame_counter, s->raw_state,
                    s->slider_pos / TTP_SLIDER_SCALE, s->slider_pos % TTP_SLIDER_SCALE, s->func_pressed);
        }

        // 周期性原始状态转储 (~500ms)
        if (++dump_ticks >= 100) {
            dump_ticks = 0;
            TTP_LOG("dump: raw=0x%04x slider=%d.%02d dir=%u spd=%d fn=0x%02x jp=0x%02x jr=0x%02x\r\n", s->raw_state,
                    s->slider_pos / TTP_SLIDER_SCALE, s->slider_pos % TTP_SLIDER_SCALE, (unsigned)s->slider_direction,
                    s->slider_speed, s->func_pressed, s->func_just_pressed, s->func_just_released);
        }

        // 心跳 (~2s) 确认任务活着
        if (++hb_ticks >= 400) {
            hb_ticks = 0;
            TTP_LOG("heartbeat: frames=%u ticks=%u raw=0x%04x slider=%d.%02d fn=0x%02x\r\n", (unsigned)frame_counter,
                    (unsigned)ticks, s->raw_state, s->slider_pos / TTP_SLIDER_SCALE, s->slider_pos % TTP_SLIDER_SCALE,
                    s->func_pressed);
        }

        last_raw = s->raw_state;
        last_pressed = s->func_pressed;
        (void)last_raw;
        (void)last_pressed;
        (void)ticks;

        ticks++;
        osal_msleep(sed_ws63::ttp229::POLL_INTERVAL_MS);
    }
    return nullptr;
}
