#pragma once

extern "C" {
#include "soc_osal.h"
#include "gpio.h"
#include "pinctrl.h"
}
#include "iic.hpp"

namespace sed_ws63 {
class cs43131 {
public:
private:
    iic_master iic = {GPIO_16, GPIO_15};
    static constexpr uint16_t iic_addr = 0x30;

    // CS43131 单寄存器写入格式不是 4 字节，而是 5 字节：
    // [MAP23:16] [MAP15:8] [MAP7:0] [CONTROL] [DATA]
    // 这里 CONTROL 固定用 0x00，表示 8-bit 单寄存器访问且不自增。
    // 下面这组命令默认针对 22.5792 MHz 晶振、44.1 kHz、I2S Slave、双声道 16-bit。
    // 40 MHz 不能直接作为 CS43131 的晶振或 PLL 参考输入，这一点是 PDF 的硬限制。

    // 读取中断状态 1 的前导写，用于清除 sticky bit 或轮询 XTAL_READY_INT。
    static constexpr uint8_t int_status_1_read_cmd[4] = {0x0F, 0x00, 0x00, 0x00};

    // 读取中断状态 2 的前导写，用于清除 ASP 相关 sticky bit。
    static constexpr uint8_t int_status_2_read_cmd[4] = {0x0F, 0x00, 0x01, 0x00};

    // 读取 Power Down Control 的前导写，后续需要读改写来打开 ASP/HP。
    static constexpr uint8_t power_down_ctrl_read_cmd[4] = {0x02, 0x00, 0x00, 0x00};

    // Interrupt Status 1 的 bit4 对应 XTAL_READY_INT。
    static constexpr uint8_t int_status_1_xtal_ready_mask = 0x10;

    // 配置晶振偏置电流，0x04 对应 12.5 uA，适用于 PDF 示例里的 22.5792 MHz 晶振。
    static constexpr uint8_t crystal_cmd[5] = {0x02, 0x00, 0x52, 0x00, 0x04};

    // 打开 XTAL_READY 和 XTAL_ERROR 相关中断，便于判断晶振是否真正起振成功。
    static constexpr uint8_t enable_xtal_irq_cmd[5] = {0x0F, 0x00, 0x10, 0x00, 0xE7};

    // 启动 XTAL，其他数据通路先保持关断。
    static constexpr uint8_t start_xtal_cmd[5] = {0x02, 0x00, 0x00, 0x00, 0xF6};

    // ASP 采样率设为 44.1 kHz。
    static constexpr uint8_t asp_sample_rate_44k1_cmd[5] = {0x01, 0x00, 0x0B, 0x00, 0x01};

    // XSP 保持 24-bit 默认，ASP 改成 16-bit。
    static constexpr uint8_t asp_sample_bit_size_16_cmd[5] = {0x01, 0x00, 0x0C, 0x00, 0x06};

    // 即使在 Slave 模式下，PDF 也要求把期望的 SCLK/LRCK 格式按同样方式写入。
    // ASP SCLK 分频分子 N = 1。
    static constexpr uint8_t asp_n_lsb_cmd[5] = {0x04, 0x00, 0x10, 0x00, 0x01};

    static constexpr uint8_t asp_n_msb_cmd[5] = {0x04, 0x00, 0x11, 0x00, 0x00};

    // 在 22.5792 MHz 下，为了得到 16-bit stereo 的 1.4112 MHz BCLK，这里 M = 16。
    static constexpr uint8_t asp_m_lsb_cmd[5] = {0x04, 0x00, 0x12, 0x00, 0x10};

    static constexpr uint8_t asp_m_msb_cmd[5] = {0x04, 0x00, 0x13, 0x00, 0x00};

    // 50/50 模式下，每相 16 个 BCLK，因此 LRCK high time = 15。
    static constexpr uint8_t asp_lrck_high_lsb_cmd[5] = {0x04, 0x00, 0x14, 0x00, 0x0F};

    static constexpr uint8_t asp_lrck_high_msb_cmd[5] = {0x04, 0x00, 0x15, 0x00, 0x00};

    // 一帧 32 个 BCLK，因此 LRCK period = 31。
    static constexpr uint8_t asp_lrck_period_lsb_cmd[5] = {0x04, 0x00, 0x16, 0x00, 0x1F};

    static constexpr uint8_t asp_lrck_period_msb_cmd[5] = {0x04, 0x00, 0x17, 0x00, 0x00};

    // ASP 工作在 Slave 模式，输入时钟极性按 PDF 的 I2S Slave 示例配置。
    static constexpr uint8_t asp_clock_cfg_cmd[5] = {0x04, 0x00, 0x18, 0x00, 0x0C};

    // ASP 帧格式设为 I2S，50/50，占 1 bit 延迟。
    static constexpr uint8_t asp_frame_cfg_cmd[5] = {0x04, 0x00, 0x19, 0x00, 0x0A};

    // 声道 1 从每个 frame 的第 0 个 SCLK 开始取数。
    static constexpr uint8_t asp_ch1_location_cmd[5] = {0x05, 0x00, 0x00, 0x00, 0x00};

    // 声道 2 也从各自半帧的第 0 个 SCLK 开始取数。
    static constexpr uint8_t asp_ch2_location_cmd[5] = {0x05, 0x00, 0x01, 0x00, 0x00};

    // 声道 1：低相位、16-bit、使能。
    static constexpr uint8_t asp_ch1_size_enable_cmd[5] = {0x05, 0x00, 0x0A, 0x00, 0x05};

    // 声道 2：高相位、16-bit、使能。
    static constexpr uint8_t asp_ch2_size_enable_cmd[5] = {0x05, 0x00, 0x0B, 0x00, 0x0D};

    // PCM 滤波配置：打开高通，关闭 de-emphasis。
    static constexpr uint8_t pcm_filter_cmd[5] = {0x09, 0x00, 0x00, 0x00, 0x02};

    // 声道 B 数字音量设为 0 dB。
    static constexpr uint8_t pcm_volume_b_0db_cmd[5] = {0x09, 0x00, 0x01, 0x00, 0x00};

    // 声道 A 数字音量设为 0 dB。
    static constexpr uint8_t pcm_volume_a_0db_cmd[5] = {0x09, 0x00, 0x02, 0x00, 0x00};

    // PCM 路径控制，沿用 PDF 官方启动序列设置软启动和 automute 行为。
    static constexpr uint8_t pcm_path_ctrl_1_cmd[5] = {0x09, 0x00, 0x03, 0x00, 0xEC};

    // 不反相、不交换左右声道、不复制声道。
    static constexpr uint8_t pcm_path_ctrl_2_cmd[5] = {0x09, 0x00, 0x04, 0x00, 0x00};

    // Class-H 耳放工作参数，沿用 PDF 官方推荐值。
    static constexpr uint8_t class_h_ctrl_cmd[5] = {0x0B, 0x00, 0x00, 0x00, 0x1E};

    // HP 输出幅度设为 1.732 Vrms 全幅输出。
    static constexpr uint8_t hp_output_full_scale_cmd[5] = {0x08, 0x00, 0x00, 0x00, 0x30};

    // 先配置耳机检测参数，但先不使能检测。
    static constexpr uint8_t hp_detect_cfg_cmd[5] = {0x0D, 0x00, 0x00, 0x00, 0x04};

    // 再正式打开 HP detect。
    static constexpr uint8_t hp_detect_enable_cmd[5] = {0x0D, 0x00, 0x00, 0x00, 0xC4};

    // 打开耳机插拔相关中断。
    static constexpr uint8_t enable_hp_irq_cmd[5] = {0x0F, 0x00, 0x10, 0x00, 0x87};

    // 不使能耳机
    static constexpr uint8_t disable_hp_cmd[5] = {0x0D, 0x00, 0x00, 0x00, 0x04};

    // 打开 ASP 溢出、LRCK 错误、无 LRCK 等中断。
    static constexpr uint8_t enable_asp_irq_cmd[5] = {0x0F, 0x00, 0x11, 0x00, 0x07};

    // XTAL_READY 后，将内部 MCLK 切换到 XTAL，且目标频率为 22.5792 MHz。
    static constexpr uint8_t switch_mclk_to_xtal_cmd[5] = {0x01, 0x00, 0x06, 0x00, 0x04};

    // PCM 路径上电前的 pop-free 预处理 1。
    static constexpr uint8_t pcm_popfree_stage1_cmd[5] = {0x01, 0x00, 0x10, 0x00, 0x99};

    // PCM 路径上电前的 pop-free 预处理 2。
    static constexpr uint8_t pcm_popfree_stage2_cmd[5] = {0x08, 0x00, 0x32, 0x00, 0x20};

    // 上电结束后恢复默认设置。
    static constexpr uint8_t pcm_popfree_restore_1_cmd[5] = {0x08, 0x00, 0x32, 0x00, 0x00};

    static constexpr uint8_t pcm_popfree_restore_2_cmd[5] = {0x01, 0x00, 0x10, 0x00, 0x00};

    // 这两个不是直接写命令，而是对 0x20000 读改写时要用到的掩码：
    // 先 current_value & 0xBF 打开 ASP，再 current_value & 0xEF 打开 HP/DAC。
    static constexpr uint8_t power_down_enable_asp_mask = 0xBF;
    static constexpr uint8_t power_down_enable_hp_mask = 0xEF;

public:
    cs43131();
    ~cs43131() = default;

private:
    void cs43131_init();
};
} // namespace sed_ws63