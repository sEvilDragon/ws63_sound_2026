#include "cs43131.hpp"

namespace sed_ws63 {
cs43131::cs43131()
{
    osal_msleep(200); // 等待电源稳定
    cs43131_init();
    osal_msleep(200); // 等待 CS43131 内部稳定
    // 拉高通讯引脚，通知iis可以启动
    //     uapi_pin_set_mode(GPIO_14, PIN_MODE_0);
    //     uapi_gpio_set_dir(GPIO_14, GPIO_DIRECTION_OUTPUT);
    //     uapi_gpio_set_val(GPIO_14, GPIO_LEVEL_HIGH);
}

void cs43131::cs43131_init()
{
    // 设置晶振偏置
    // iic.iic_master_write(const_cast<uint8_t *>(crystal_cmd), sizeof(crystal_cmd), iic_addr);
    // 读取中断状态1
    uint8_t status = 0;
    iic.iic_master_read(const_cast<uint8_t *>(int_status_1_read_cmd), sizeof(int_status_1_read_cmd), &status, 1,
                        iic_addr);
    // 打开晶振相关中断
    // iic.iic_master_write(const_cast<uint8_t *>(enable_xtal_irq_cmd), sizeof(enable_xtal_irq_cmd), iic_addr);
    // 启动晶振
    // iic.iic_master_write(const_cast<uint8_t *>(start_xtal_cmd), sizeof(start_xtal_cmd), iic_addr);
    // 开启44100Hz采样率
    iic.iic_master_write(const_cast<uint8_t *>(asp_sample_rate_44k1_cmd), sizeof(asp_sample_rate_44k1_cmd), iic_addr);
    // 读取一次来判断iic是否写入成功
    uint8_t nn[] = {0x01, 0x00, 0x0B, 0x00};
    iic.iic_master_read(nn, sizeof(nn), &status, 1, iic_addr);
    osal_printk("CS43131 ASP sample rate set status: 0x%02X\n, 应该为0x01", status);
    // 设置16-bit采样位宽
    iic.iic_master_write(const_cast<uint8_t *>(asp_sample_bit_size_16_cmd), sizeof(asp_sample_bit_size_16_cmd),
                         iic_addr);
    // N = 1
    iic.iic_master_write(const_cast<uint8_t *>(asp_n_lsb_cmd), sizeof(asp_n_lsb_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(asp_n_msb_cmd), sizeof(asp_n_msb_cmd), iic_addr);
    // M = 16
    iic.iic_master_write(const_cast<uint8_t *>(asp_m_lsb_cmd), sizeof(asp_m_lsb_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(asp_m_msb_cmd), sizeof(asp_m_msb_cmd), iic_addr);
    // LRCK high time = 15
    iic.iic_master_write(const_cast<uint8_t *>(asp_lrck_high_lsb_cmd), sizeof(asp_lrck_high_lsb_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(asp_lrck_high_msb_cmd), sizeof(asp_lrck_high_msb_cmd), iic_addr);
    // LRCK period = 31
    iic.iic_master_write(const_cast<uint8_t *>(asp_lrck_period_lsb_cmd), sizeof(asp_lrck_period_lsb_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(asp_lrck_period_msb_cmd), sizeof(asp_lrck_period_msb_cmd), iic_addr);
    // ASP 时钟配置
    iic.iic_master_write(const_cast<uint8_t *>(asp_clock_cfg_cmd), sizeof(asp_clock_cfg_cmd), iic_addr);
    // ASP 帧格式配置
    iic.iic_master_write(const_cast<uint8_t *>(asp_frame_cfg_cmd), sizeof(asp_frame_cfg_cmd), iic_addr);
    // 声道起点
    iic.iic_master_write(const_cast<uint8_t *>(asp_ch1_location_cmd), sizeof(asp_ch1_location_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(asp_ch2_location_cmd), sizeof(asp_ch2_location_cmd), iic_addr);
    // 声道大小和使能
    iic.iic_master_write(const_cast<uint8_t *>(asp_ch1_size_enable_cmd), sizeof(asp_ch1_size_enable_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(asp_ch2_size_enable_cmd), sizeof(asp_ch2_size_enable_cmd), iic_addr);
    // PCM 滤波配置
    iic.iic_master_write(const_cast<uint8_t *>(pcm_filter_cmd), sizeof(pcm_filter_cmd), iic_addr);
    // PCM 音量配置
    iic.iic_master_write(const_cast<uint8_t *>(pcm_volume_b_0db_cmd), sizeof(pcm_volume_b_0db_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(pcm_volume_a_0db_cmd), sizeof(pcm_volume_a_0db_cmd), iic_addr);
    // PCM 路径控制
    iic.iic_master_write(const_cast<uint8_t *>(pcm_path_ctrl_1_cmd), sizeof(pcm_path_ctrl_1_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(pcm_path_ctrl_2_cmd), sizeof(pcm_path_ctrl_2_cmd), iic_addr);
    // Class-H 耳放参数配置
    iic.iic_master_write(const_cast<uint8_t *>(class_h_ctrl_cmd), sizeof(class_h_ctrl_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(hp_output_full_scale_cmd), sizeof(hp_output_full_scale_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(hp_detect_cfg_cmd), sizeof(hp_detect_cfg_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(hp_detect_enable_cmd), sizeof(hp_detect_enable_cmd), iic_addr);
    // 清空中断状态
    iic.iic_master_read(const_cast<uint8_t *>(int_status_1_read_cmd), sizeof(int_status_1_read_cmd), &status, 1,
                        iic_addr);
    iic.iic_master_read(const_cast<uint8_t *>(int_status_2_read_cmd), sizeof(int_status_2_read_cmd), &status, 1,
                        iic_addr);
    // ASP 中断配置
    iic.iic_master_write(const_cast<uint8_t *>(enable_asp_irq_cmd), sizeof(enable_asp_irq_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(enable_hp_irq_cmd), sizeof(enable_hp_irq_cmd), iic_addr);
    // 轮询 XTAL ready，再切换内部时钟到 XTAL。
    // for (uint8_t retry = 0; retry < 50; ++retry) {
    //     iic.iic_master_read(const_cast<uint8_t *>(int_status_1_read_cmd), sizeof(int_status_1_read_cmd), &status, 1,
    //                         iic_addr);
    //     if ((status & int_status_1_xtal_ready_mask) != 0) {
    //         break;
    //     }
    //     osal_msleep(1);
    //}
    // 切换时钟源
    // iic.iic_master_write(const_cast<uint8_t *>(switch_mclk_to_xtal_cmd), sizeof(switch_mclk_to_xtal_cmd), iic_addr);
    osal_msleep(1);
    // pop_free 设置，沿用 PDF 推荐值。
    iic.iic_master_write(const_cast<uint8_t *>(pcm_popfree_stage1_cmd), sizeof(pcm_popfree_stage1_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(pcm_popfree_stage2_cmd), sizeof(pcm_popfree_stage2_cmd), iic_addr);
    // 读取电源控制寄存器，再按 PDF 的读改写顺序打开 ASP 和 HP。
    uint8_t power_down_ctrl = 0;
    iic.iic_master_read(const_cast<uint8_t *>(power_down_ctrl_read_cmd), sizeof(power_down_ctrl_read_cmd),
                        &power_down_ctrl, 1, iic_addr);
    const uint8_t asp_power_ctrl = static_cast<uint8_t>(power_down_ctrl & power_down_enable_asp_mask);
    // 开启 ASP 电源，Slave 模式下 ASP 只接收外部 BCLK/LRCK。
    uint8_t power_asp_cmd[5] = {0x02, 0x00, 0x00, 0x00, asp_power_ctrl};
    iic.iic_master_write(power_asp_cmd, sizeof(power_asp_cmd), iic_addr);
    // 开启 HP 电源
    uint8_t power_hp_cmd[5] = {0x02, 0x00, 0x00, 0x00,
                               static_cast<uint8_t>(asp_power_ctrl & power_down_enable_hp_mask)};
    iic.iic_master_write(power_hp_cmd, sizeof(power_hp_cmd), iic_addr);
    // 延时
    osal_msleep(12);
    // 恢复默认设置
    iic.iic_master_write(const_cast<uint8_t *>(pcm_popfree_restore_1_cmd), sizeof(pcm_popfree_restore_1_cmd), iic_addr);
    iic.iic_master_write(const_cast<uint8_t *>(pcm_popfree_restore_2_cmd), sizeof(pcm_popfree_restore_2_cmd), iic_addr);
}

} // namespace sed_ws63