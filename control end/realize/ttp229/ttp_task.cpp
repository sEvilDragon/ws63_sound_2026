#include "ttp_task.hpp"

extern "C" {
#include "gpio.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "app_init.h"
}

// 详细诊断开关: 1 = 输出逐帧串行原始值; 0 = 仅输出触摸状态变化
// 默认 0, 调试 bit-bang 信号时手动改为 1 重新编译
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
      m_button_debounce(0),
      m_button_stable(false)
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

void ttp229::process_button(uint16_t raw)
{
    bool raw_pressed = (raw & BUTTON_RAW_MASK) != 0;
    bool previous = m_button_stable;

    m_state.button_just_pressed = 0;
    m_state.button_just_released = 0;

    if (raw_pressed == m_button_stable) {
        m_button_debounce = 0;
    } else {
        int required_ticks = raw_pressed ? BUTTON_PRESS_DEBOUNCE_TICKS : BUTTON_RELEASE_DEBOUNCE_TICKS;
        if (++m_button_debounce >= required_ticks) {
            m_button_stable = raw_pressed;
            m_button_debounce = 0;
        }
    }

    m_state.button_pressed = m_button_stable ? 1 : 0;
    m_state.button_just_pressed = (!previous && m_button_stable) ? 1 : 0;
    m_state.button_just_released = (previous && !m_button_stable) ? 1 : 0;
}

void ttp229::update()
{
    uint16_t raw = 0;
    if (!read_touch(&raw)) {
        return;
    }

    m_state.raw_state = raw;
    process_button(raw);
}

} // namespace sed_ws63

static sed_ws63::ttp229 *g_ttp = nullptr;
static ttp_state_t g_ttp_state;

const ttp_state_t *ttp_get_state(void)
{
    return &g_ttp_state;
}

static int ttp_single_pad_from_mask(uint16_t mask)
{
    if (mask == 0) {
        return 0;
    }
    if ((mask & (uint16_t)(mask - 1)) != 0) {
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        if (mask & (uint16_t)(1u << i)) {
            return i + 1;
        }
    }
    return -1;
}

static void ttp_debug_raw_change(const ttp_state_t *s)
{
    static bool inited = false;
    static uint16_t last_raw = 0;

    if (!s) {
        return;
    }
    if (inited && s->raw_state == last_raw) {
        return;
    }
    inited = true;
    last_raw = s->raw_state;

    int pad = ttp_single_pad_from_mask(s->raw_state);
    osal_printk("[TTP] touch=0x%04x pad=%d button=%u just=%u\r\n",
                (unsigned)s->raw_state, pad, (unsigned)s->button_pressed,
                (unsigned)s->button_just_pressed);
}
void *ttp_task(void *arg)
{
    (void)arg;

    static sed_ws63::ttp229 s_ttp;
    g_ttp = &s_ttp;

    while (true) {
        g_ttp->update();
        g_ttp_state = g_ttp->get_state();
        ttp_debug_raw_change(&g_ttp_state);

        osal_msleep(sed_ws63::ttp229::POLL_INTERVAL_MS);
    }
    return nullptr;
}
