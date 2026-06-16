#include "iis.hpp"
#include <cmath>

volatile int iis::write_idx = 0;
volatile int iis::read_idx = 0;
volatile int iis::pending_frames = 0;
volatile uint32_t iis::write_offset = 0;
std::array<void *, 100> iis::raw_buffers;
std::array<int16_t *, 100> iis::dma_buffers;
uint8_t iis::dma_channel = 0;
bool iis::is_ready = false;
int16_t iis::last_left_sample = 0;
int16_t iis::last_right_sample = 0;
float iis::biquad_bx1_l = 0;
float iis::biquad_bx2_l = 0;
float iis::biquad_by1_l = 0;
float iis::biquad_by2_l = 0;
float iis::biquad_bx1_r = 0;
float iis::biquad_bx2_r = 0;
float iis::biquad_by1_r = 0;
float iis::biquad_by2_r = 0;

const uint16_t iis::volume_gain_table[101] = {
        0,   110,   116,   123,   130,   138,   146,   155,   164,   174,
      184,   195,   207,   219,   232,   246,   260,   276,   292,   309,
      328,   347,   368,   389,   413,   437,   463,   490,   519,   550,
      583,   617,   654,   693,   734,   777,   823,   872,   923,   978,
     1036,  1098,  1163,  1232,  1304,  1382,  1464,  1550,  1642,  1740,
     1843,  1952,  2067,  2190,  2320,  2457,  2603,  2757,  2920,  3093,
     3277,  3471,  3677,  3894,  4125,  4370,  4628,  4903,  5193,  5501,
     5827,  6172,  6538,  6925,  7336,  7770,  8231,  8718,  9235,  9782,
    10362, 10976, 11626, 12315, 13045, 13818, 14636, 15504, 16422, 17395,
    18426, 19518, 20675, 21900, 23197, 24572, 26028, 27570, 29204, 30934,
    32767
};


iis::iis()
{
    osal_msleep(8000); // 等待系统稳定，确保DMA和I2S驱动准备就绪
    pin_init();
    i2s_dma_init_1();
    i2s_init();
    i2s_dma_init_2();
    set_rate_of_iis(i2s_sample_rate); // dma_config 会用 FREQ_OF_NEED=32 覆盖 BCLK，必须在其后重置为48kHz
    sem_mutex_init();
    dma_lli_init();
}

iis::~iis()
{
    // 释放分配的DMA缓冲区
    for (int i = 0; i < buffer_num; i++) {
        if (raw_buffers[i] != nullptr) {
            osal_kfree(raw_buffers[i]);
            raw_buffers[i] = nullptr;
            dma_buffers[i] = nullptr;
        }
    }
}

void iis::pin_init()
{
    // 配置引脚功能
    uapi_pin_set_mode(mcsl_pin, PIN_MODE_4);
    uapi_pin_set_mode(sclk_pin, PIN_MODE_4);
    uapi_pin_set_mode(lrclk_pin, PIN_MODE_4);
    uapi_pin_set_mode(dataout_pin, PIN_MODE_4);

    // 配置引脚方向
    uapi_pin_set_ds(mcsl_pin, PIN_DS_4);
    uapi_pin_set_ds(sclk_pin, PIN_DS_4);
    uapi_pin_set_ds(lrclk_pin, PIN_DS_4);
    uapi_pin_set_ds(dataout_pin, PIN_DS_4);

    // LRCLK/DIN引脚官方移植层关闭上下拉，这里保持一致
    uapi_pin_set_pull(lrclk_pin, PIN_PULL_TYPE_DISABLE);
    uapi_pin_set_pull(dataout_pin, PIN_PULL_TYPE_DISABLE);
}

void iis::i2s_dma_init_1()
{
    // 使能DMA，该操作原先放在iis启动之前，这里同样选择分离
    uapi_dma_init();
    uapi_dma_open();
}

void iis::i2s_init()
{
    uapi_i2s_init(i2s_num, nullptr);

    i2s_config_t i2s_config;
    memset(&i2s_config, 0, sizeof(i2s_config_t));
    i2s_config.drive_mode = drive_mode;
    i2s_config.transfer_mode = transfer_mode;
    i2s_config.data_width = data_width;
    i2s_config.channels_num = channel_num;
    i2s_config.timing = timing;
    i2s_config.clk_edge = clk_edge;
    i2s_config.div_number = div_num;
    i2s_config.number_of_channels = channel_num_true;

    errcode_t ret1 = uapi_i2s_set_config(i2s_num, &i2s_config);
    if (ret1 != ERRCODE_SUCC) {
        osal_printk("I2S配置设置失败1，错误码：%u\n", ret1);
    }

    // 使能I2S时钟（先开时钟，再配DMA）
    uapi_i2s_set_crg_clock_enable(i2s_num, true);
    osal_msleep(10);
}

void iis::set_rate_of_iis(i2s_sample_rate_t rate)
{
    errcode_t ret = uapi_i2s_set_sample_rate(i2s_num, rate);
    if (ret != ERRCODE_SUCC) {
        osal_printk("I2S采样率设置失败，错误码：%u\n", ret);
    }
}

void iis::i2s_dma_init_2()
{
    i2s_dma_attr_t dma_attr;
    memset(&dma_attr, 0, sizeof(i2s_dma_attr_t));
    dma_attr.tx_dma_enable = tx_dma_enable;
    dma_attr.tx_int_threshold = tx_threshold;
    dma_attr.rx_dma_enable = rx_dma_enable;
    dma_attr.rx_int_threshold = rx_threshold;

    errcode_t ret = uapi_i2s_dma_config(i2s_num, &dma_attr);
    if (ret != ERRCODE_SUCC) {
        osal_printk("I2S DMA配置设置失败，错误码：%u\n", ret);
    }
}

void iis::sem_mutex_init()
{
    raw_buffers.fill(nullptr);
    dma_buffers.fill(nullptr);

    // 海思涉及cache操作，需要手动对齐Cache Line
    for (int i = 0; i < buffer_num; i++) {
        uint32_t buf_size = buffer_size * sizeof(uint16_t) + cache_size; // 额外空间用于对齐
        raw_buffers[i] = osal_kmalloc(buf_size, OSAL_GFP_DMA);

        // 分配失败则清理已分配的资源并返回
        if (raw_buffers[i] == nullptr) {
            for (int j = 0; j < i; j++) {
                osal_kfree(raw_buffers[j]);
                raw_buffers[j] = nullptr;
                dma_buffers[j] = nullptr;
            }
            return;
        }

        // 手动对齐内存地址到Cache Line大小
        uintptr_t addr = (uintptr_t)raw_buffers[i];
        // 对齐到下一个cache line边界
        // 后面的反码实现了去除了所有低cache_size位的地址，从而实现了向下对齐；加上cache_size - 1则实现了向上对齐
        uintptr_t aligned_addr = (addr + cache_size - 1) & ~(cache_size - 1);

        dma_buffers[i] = (uint16_t *)aligned_addr;
        for (int j = 0; j < buffer_size; j++) {
            dma_buffers[i][j] = 0; // 初始化缓冲区数据为0
        }
    }
}

void iis::i2s_send_callback(uint8_t intr, uint8_t channel, uintptr_t arg)
{
    unused(channel);
    unused(arg);

    if (intr == HAL_DMA_INTERRUPT_TFR) {

        uint32_t new_read_idx = (read_idx + 1) % buffer_num; // 计算下一个读取索引
        if (pending_frames > 0)
            pending_frames--; // 发送了一帧数据，待处理帧数减1

        data_clear_one(read_idx); // 清理刚发送完的缓冲区，DMA下次经过此槽时播平滑保持采样
        read_idx = new_read_idx;  // 更新读取索引

        // 在消耗侧检查缓冲是否耗尽：pending_frames 刚被减，此处最准确
        if (is_ready && pending_frames <= min_buffer_num) {
            hal_sio_set_tx_enable(i2s_num, 0);
            is_ready = false;
        }
    }
}

void iis::dma_lli_init()
{
    dma_channel = uapi_dma_get_lli_channel(0, HAL_DMA_HANDSHAKING_MAX_NUM);

    // 确保通道号有效
    if (dma_channel >= DMA_CHANNEL_MAX_NUM) {
        osal_printk("获取LLI通道失败\n");
        return;
    }

    dma_ch_user_peripheral_config_t dma_ch_config;
    memset(&dma_ch_config, 0, sizeof(dma_ch_user_peripheral_config_t));
    dma_ch_config.dest = (uint32_t)i2s_porting_tx_merge_data_addr_get(i2s_num);
    dma_ch_config.transfer_num = transfer_size;
    dma_ch_config.src_handshaking = src_handshaking;
    dma_ch_config.dest_handshaking = dest_handshaking;
    dma_ch_config.trans_type = transfer_type;
    dma_ch_config.trans_dir = trans_dir;
    dma_ch_config.priority = priority;
    dma_ch_config.src_width = src_width;
    dma_ch_config.dest_width = dest_width;
    dma_ch_config.burst_length = burst_length;
    dma_ch_config.src_increment = src_increment;
    dma_ch_config.dest_increment = dest_increment;
    dma_ch_config.protection = protection;

    for (int i = 0; i < buffer_num; i++) {
        dma_ch_config.src = (uint32_t)(uintptr_t)dma_buffers[i];
        errcode_t err = uapi_dma_configure_peripheral_transfer_lli(dma_channel, &dma_ch_config, i2s_send_callback);
        if (err != 0) {
            // SED_LOG : DMA LLI配置失败
            osal_printk("DMA LLI配置失败，错误码: %u\n", err);
        }
    }

    osal_flush_cache();

    errcode_t start_ret = uapi_dma_enable_lli(dma_channel, i2s_send_callback, (uintptr_t)nullptr);
    if (start_ret != ERRCODE_SUCC) {
        osal_printk("DMA LLI启动失败，错误码: 0x%x\n", start_ret);
    }

    osal_flush_cache();

    // SED : 情况
    hal_sio_set_crg_clock_enable(i2s_num, true);
    // TX 不立即激活，等待 data_write 积累足够帧数后再开启，避免缓冲区不足时播放噪声
    hal_sio_set_tx_enable(i2s_num, 0);
}

void iis::data_write(const int16_t *data, uint32_t size, uint8_t volume, uint8_t bass)
{
    if (size % 2 != 0) {
        size--;
        return;
    }

    reduce_buffer_if_needed(size);

    float bass_b0 = 0, bass_b1 = 0, bass_b2 = 0, bass_a1 = 0, bass_a2 = 0;
    if (bass > 0) {
        float A = powf(10.0f, (float)bass * 0.12f / 20.0f);
        float w0 = 2.0f * 3.14159265f * 150.0f / 44100.0f;
        float cs = cosf(w0);
        float sn = sinf(w0);
        float al = sn / 1.41421356f;
        float sqA = 2.0f * sqrtf(A) * al;
        float a0 = (A + 1.0f) + (A - 1.0f) * cs + sqA;
        bass_b0 = (A * ((A + 1.0f) - (A - 1.0f) * cs + sqA)) / a0;
        bass_b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs)) / a0;
        bass_b2 = (A * ((A + 1.0f) - (A - 1.0f) * cs - sqA)) / a0;
        bass_a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cs)) / a0;
        bass_a2 = ((A + 1.0f) + (A - 1.0f) * cs - sqA) / a0;
    }

    while (size > 0) {
        if (write_offset == 0 && pending_frames >= (int)buffer_num) {
            return;
        }

        uint32_t space = buffer_size - write_offset;
        uint32_t to_copy = (size < space) ? size : space;
        memcpy(&dma_buffers[write_idx][write_offset], data, to_copy * sizeof(int16_t));

        if (volume < 100) {
            uint16_t gain = volume_gain_table[volume > 100 ? 100 : volume];
            int16_t *dst = &dma_buffers[write_idx][write_offset];
            for (uint32_t i = 0; i < to_copy; i++) {
                dst[i] = (int16_t)(((int32_t)dst[i] * gain) >> 15);
            }
        }

        if (bass > 0) {
            int16_t *dst = &dma_buffers[write_idx][write_offset];
            for (uint32_t i = 0; i < to_copy; i += 2) {
                float x = (float)dst[i];
                float y = bass_b0 * x + bass_b1 * biquad_bx1_l + bass_b2 * biquad_bx2_l - bass_a1 * biquad_by1_l - bass_a2 * biquad_by2_l;
                biquad_bx2_l = biquad_bx1_l; biquad_bx1_l = x;
                biquad_by2_l = biquad_by1_l; biquad_by1_l = y;
                if (y > 32767.0f) y = 32767.0f; else if (y < -32768.0f) y = -32768.0f;
                dst[i] = (int16_t)y;
                if (i + 1 < to_copy) {
                    x = (float)dst[i + 1];
                    y = bass_b0 * x + bass_b1 * biquad_bx1_r + bass_b2 * biquad_bx2_r - bass_a1 * biquad_by1_r - bass_a2 * biquad_by2_r;
                    biquad_bx2_r = biquad_bx1_r; biquad_bx1_r = x;
                    biquad_by2_r = biquad_by1_r; biquad_by1_r = y;
                    if (y > 32767.0f) y = 32767.0f; else if (y < -32768.0f) y = -32768.0f;
                    dst[i + 1] = (int16_t)y;
                }
            }
        }

        last_left_sample = dma_buffers[write_idx][write_offset + to_copy - 2];
        last_right_sample = dma_buffers[write_idx][write_offset + to_copy - 1];

        data += to_copy;
        size -= to_copy;
        write_offset += to_copy;

        if (write_offset >= buffer_size) {
            osal_dcache_region_clean(dma_buffers[write_idx], buffer_size * sizeof(uint16_t));
            write_offset = 0;
            uint32_t irq = osal_irq_lock();
            pending_frames++;
            write_idx = (write_idx + 1) % buffer_num;
            if (!is_ready && pending_frames >= prebuffer_num) {
                is_ready = true;
                hal_sio_set_tx_enable(i2s_num, 1);
            }
            osal_irq_restore(irq);
        }
    }
}

void iis::data_clear()
{
    hal_sio_set_tx_enable(i2s_num, 0);

    for (int i = 0; i < buffer_num; i++) {
        memset(dma_buffers[i], 0, buffer_size * sizeof(uint16_t));
        // DMA 直接读物理内存，必须 writeback 否则 DMA 仍读到旧音频数据
        osal_dcache_region_clean(dma_buffers[i], buffer_size * sizeof(uint16_t));
    }
    // 不能将 read_idx 归零：DMA LLI 硬件节点不会因软件重置而归零。
    // 若强制 read_idx=0 而 DMA 停在节点 N，重连后 DMA 从 N 往后播零值槽，
    // 每播一槽回调递减 pending_frames，在到达新数据前就触发 TX 关闭 → 永久静音。
    // 正确做法：保留 read_idx 与硬件同步，write_idx 对齐到 read_idx，
    // 新数据从 DMA 当前位置起填入，pending_frames 与实际可用槽一一对应。
    uint32_t irq = osal_irq_lock();
    write_idx = read_idx;
    write_offset = 0;
    last_left_sample = 0;
    last_right_sample = 0;
    biquad_bx1_l = biquad_bx2_l = biquad_by1_l = biquad_by2_l = 0;
    biquad_bx1_r = biquad_bx2_r = biquad_by1_r = biquad_by2_r = 0;
    pending_frames = 0;
    is_ready = false;
    osal_irq_restore(irq);
}

void iis::data_clear_one(int index)
{
    if (index < 0 || index >= (int)buffer_num) {
        return; // 索引越界，直接返回
    }
    // 欠载时用最后采样值填充，较纯静音更不易产生突兀爆音。
    for (uint32_t i = 0; i + 1 < buffer_size; i += 2) {
        dma_buffers[index][i] = last_left_sample;
        dma_buffers[index][i + 1] = last_right_sample;
    }
    // DMA 直接读物理内存，memset 只写 CPU cache，不 writeback 则 DMA 读到物理内存里的旧音频，
    // 产生每次完全相同的重复杂音。
    osal_dcache_region_clean(dma_buffers[index], buffer_size * sizeof(uint16_t));
}

void iis::fill_buffer_if_needed()
{
    if (pending_frames < if_fill_num) {
        // 重复一定帧数，抗衡抖动
        static std::array<int16_t, if_small_num * 2> fill_data = {0};
        for (int i = 0; i < if_small_num; i++) {
            fill_data[i * 2] = last_left_sample;
            fill_data[i * 2 + 1] = last_right_sample;
        }
        data_write(fill_data.data(), if_small_num * 2, 100, 0);
    }
}

void iis::reduce_buffer_if_needed(uint32_t &size)
{
    if (pending_frames > if_reduce_num) {
        // 待处理帧过多时，丢弃部分数据，避免积压过多帧导致长时间高延迟
        uint32_t drop_size = if_small_num * 2; // 每次丢弃 if_small_num 帧
        if (size > drop_size) {
            size -= drop_size;
        }
    }
}
