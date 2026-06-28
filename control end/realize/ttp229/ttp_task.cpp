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

#if TTP_VERBOSE
#define TTP_VLOG(fmt, ...) osal_printk("[TTP229] " fmt, ##__VA_ARGS__)
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
      m_slider_prev(0),
      m_slider_integrator(0),
      m_func_debounce{},
      m_func_hold{},
      m_func_stable{},
      m_func_prev_raw{}
{
    init_serial();
}

void ttp229::init_serial()
{
    uapi_pin_set_mode(SCL_PIN, PIN_MODE_0);
    uapi_pin_set_mode(SDA_PIN, PIN_MODE_0);
    uapi_gpio_set_dir(SCL_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_dir(SDA_PIN, GPIO_DIRECTION_INPUT);
    uapi_gpio_set_val(SCL_PIN, GPIO_LEVEL_HIGH);
    osal_msleep(2);

    uint16_t sample = 0;
    if (read_serial(&sample)) {
        m_healthy = true;
    }
    osal_printk("TTP:%d\r\n", m_healthy ? 1 : 0);
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

    // 位序翻转: 芯片 LSB-first 输出, 翻转为 MSB-first 使 pin 号归正
    result = (uint16_t)(((result & 0x00FF) << 8) | ((result & 0xFF00) >> 8));
    result = (uint16_t)(((result & 0x0F0F) << 4) | ((result & 0xF0F0) >> 4));
    result = (uint16_t)(((result & 0x3333) << 2) | ((result & 0xCCCC) >> 2));
    result = (uint16_t)(((result & 0x5555) << 1) | ((result & 0xAAAA) >> 1));

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

// 找 uint8_t 中最高置位的位置, 无置位返回 -1
static int bit_high(uint8_t x)
{
    if (x == 0)
        return -1;
    int n = 7;
    while (!(x & 0x80)) {
        x <<= 1;
        n--;
    }
    return n;
}
// 找 uint8_t 中最低置位的位置, 无置位返回 8
static int bit_low(uint8_t x)
{
    if (x == 0)
        return 8;
    int n = 0;
    while (!(x & 1)) {
        x >>= 1;
        n++;
    }
    return n;
}

void ttp229::process_slider(uint16_t raw)
{
    // === 二进制状态提取 ===
    uint8_t cur = 0;
    for (int i = 0; i < 8; i++) {
        if (is_pad_touched(raw, PAD_SLIDER[i])) {
            cur |= (uint8_t)(1 << i);
        }
    }

    // === 无触摸 → 全部复位 ===
    if (cur == 0) {
        m_slider_prev = 0;
        m_slider_integrator = 0;
        m_state.slider_pos = TTP_SLIDER_NO_POS;
        m_state.slider_direction = TTP_SWIPE_NONE;
        m_state.slider_speed = 0;
        return;
    }

    // === 首次触摸 ===
    if (m_slider_prev == 0) {
        m_slider_prev = cur;
        m_slider_integrator = 0;
        m_state.slider_pos = 0; // 由 UI 设 anchor
        m_state.slider_direction = TTP_SWIPE_NONE;
        m_state.slider_speed = 0;
        return;
    }

    // === pad 集合无变化 → 积分器衰减 ===
    if (cur == m_slider_prev) {
        if (m_slider_integrator > 0) {
            m_slider_integrator--;
            if (m_slider_integrator == 0)
                m_state.slider_direction = TTP_SWIPE_NONE;
        } else if (m_slider_integrator < 0) {
            m_slider_integrator++;
            if (m_slider_integrator == 0)
                m_state.slider_direction = TTP_SWIPE_NONE;
        }
        m_state.slider_speed = 0;
        return;
    }

    // === 抬手再放检测: pad 集合无交集 且 距离 > 3 pad → 复位 ===
    if ((cur & m_slider_prev) == 0) {
        int lo_cur = bit_low(cur);
        int hi_cur = bit_high(cur);
        int lo_prv = bit_low(m_slider_prev);
        int hi_prv = bit_high(m_slider_prev);
        int dist = (lo_cur > hi_prv) ? (lo_cur - hi_prv) : (lo_prv > hi_cur) ? (lo_prv - hi_cur) : 0;
        if (dist > 3) { // 跳跃 > 3 pad → 抬手再放
            m_slider_prev = cur;
            m_slider_integrator = 0;
            m_state.slider_direction = TTP_SWIPE_NONE;
            m_state.slider_speed = 0;
            return;
        }
        // 否则是单键模式正常滑动, 继续进入/离开分析
    }

    // === 进入 / 离开 位集 ===
    uint8_t entered = cur & ~m_slider_prev; // 新按下
    uint8_t left = m_slider_prev & ~cur;    // 新释放

    int hi_ent = bit_high(entered);
    int lo_ent = bit_low(entered);
    int hi_lft = bit_high(left);
    int lo_lft = bit_low(left);

    // 帧内位移方向 (加权)
    int frame_dir = 0;
    int frame_mag = 0;

    if (entered && left) {
        // 既有进入又有离开: 比较两组的中心
        int ent_ctr = hi_ent + lo_ent; // ×2 省去, 不影响比较
        int lft_ctr = hi_lft + lo_lft;
        frame_dir = ent_ctr - lft_ctr;
        frame_mag = (hi_ent >= lo_lft) ? (hi_ent - lo_lft + 1) : (lo_lft - hi_ent + 1);
    } else if (entered) {
        // 仅有进入: 手指在扩张
        // 与当前保持的 pads 比较
        uint8_t stay = cur & m_slider_prev;
        int hi_stay = bit_high(stay);
        int lo_stay = bit_low(stay);
        if (hi_stay >= 0) {
            if (lo_ent > hi_stay) {
                frame_dir = +1; // 向右扩张
                frame_mag = lo_ent - hi_stay;
            } else if (hi_ent < lo_stay) {
                frame_dir = -1; // 向左扩张
                frame_mag = lo_stay - hi_ent;
            }
        }
    } else if (left) {
        // 仅有离开: 手指在收缩
        uint8_t stay = cur & m_slider_prev;
        int hi_stay = bit_high(stay);
        int lo_stay = bit_low(stay);
        if (hi_stay >= 0) {
            if (lo_lft > hi_stay) {
                frame_dir = -1; // 右侧离开 = 向左收缩
                frame_mag = lo_lft - hi_stay;
            } else if (hi_lft < lo_stay) {
                frame_dir = +1; // 左侧离开 = 向右收缩
                frame_mag = lo_stay - hi_lft;
            }
        }
    }

    // === 积分器累积 / 衰减 ===
    if (frame_dir > 0) {
        m_slider_integrator += frame_mag * TTP_SLIDER_SCALE;
        if (m_slider_integrator > SLIDER_INTEGRATOR_MAX)
            m_slider_integrator = SLIDER_INTEGRATOR_MAX;
    } else if (frame_dir < 0) {
        m_slider_integrator -= frame_mag * TTP_SLIDER_SCALE;
        if (m_slider_integrator < -SLIDER_INTEGRATOR_MAX)
            m_slider_integrator = -SLIDER_INTEGRATOR_MAX;
    } else {
        // 方向不明 (进入和离开对称) → 衰减
        if (m_slider_integrator > 0)
            m_slider_integrator = (m_slider_integrator * 3) / 4;
        else if (m_slider_integrator < 0)
            m_slider_integrator = (m_slider_integrator * 3) / 4;
    }

    // === 积分器超过阈值 → 发射一步 (漏电积分-发射) ===
    // 右滑(向 pad 10)→ integrator < 0 → slider_pos 增加 → 音量增大
    if (m_slider_integrator < -SLIDER_FIRE_THRESHOLD) {
        m_state.slider_direction = TTP_SWIPE_RIGHT;
        m_state.slider_speed = SLIDER_STEP;
        m_state.slider_pos += SLIDER_STEP;
        m_slider_integrator += SLIDER_FIRE_THRESHOLD;
    } else if (m_slider_integrator > SLIDER_FIRE_THRESHOLD) {
        m_state.slider_direction = TTP_SWIPE_LEFT;
        m_state.slider_speed = SLIDER_STEP;
        m_state.slider_pos -= SLIDER_STEP;
        m_slider_integrator -= SLIDER_FIRE_THRESHOLD;
    } else {
        m_state.slider_direction = TTP_SWIPE_NONE;
        m_state.slider_speed = 0;
    }

    m_slider_prev = cur;
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

    while (true) {
        g_ttp->update();
        g_ttp_state = g_ttp->get_state();

        if (g_ttp_state.func_just_pressed)
            g_press_latch |= g_ttp_state.func_just_pressed;
        if (g_ttp_state.func_just_released)
            g_release_latch |= g_ttp_state.func_just_released;

        osal_msleep(sed_ws63::ttp229::POLL_INTERVAL_MS);
    }
    return nullptr;
}
