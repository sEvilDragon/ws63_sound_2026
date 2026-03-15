#include "pcm2706.hpp"

// 初始化静态变量
volatile int pcm2706::write_idx = 0;
volatile int pcm2706::read_idx = 0;
volatile int pcm2706::pending_frames = 0;

std::array<void *, 100> pcm2706::raw_buffers;
std::array<int16_t *, 100> pcm2706::dma_buffers;
osal_semaphore pcm2706::is_data_ready;
osal_semaphore pcm2706::is_buffer_empty;
osal_mutex pcm2706::dma_mutex;
uint8_t pcm2706::dma_channel = 0;

pcm2706::pcm2706()
{
    osal_msleep(8000); // 等待系统稳定，确保DMA和I2S驱动准备就绪
    pin_init();
    i2s_dma_init_1();
    i2s_init();
    i2s_dma_init_2();
    sem_mutex_init();
    dma_lli_init();
}

void pcm2706::pin_init()
{
    /* 配置引脚复用模式为 Mode 4 (I2S) */
    uapi_pin_set_mode(mcsl_pin, PIN_MODE_4);
    uapi_pin_set_mode(sclk_pin, PIN_MODE_4);
    uapi_pin_set_mode(lrclk_pin, PIN_MODE_4);
    uapi_pin_set_mode(datain_pin, PIN_MODE_4);

    /* 增加引脚驱动能力到中等 (PIN_DS_4)，避免过冲 */
    uapi_pin_set_ds(mcsl_pin, PIN_DS_4);
    uapi_pin_set_ds(sclk_pin, PIN_DS_4);
    uapi_pin_set_ds(lrclk_pin, PIN_DS_4);
    uapi_pin_set_ds(datain_pin, PIN_DS_4);

    /* 官方移植层对 LRCLK/DIN 关闭上下拉，这里保持一致 */
    uapi_pin_set_pull(lrclk_pin, PIN_PULL_TYPE_DISABLE);
    uapi_pin_set_pull(datain_pin, PIN_PULL_TYPE_DISABLE);
}

void pcm2706::i2s_dma_init_1()
{
    // 使能DMA，该操作原先放在iis启动之前，这里同样选择分离
    uapi_dma_init();
    uapi_dma_open();
}

void pcm2706::i2s_init()
{
    uapi_i2s_init(i2s_num, nullptr);

    i2s_config_t i2s_config;
    memset(&i2s_config, 0, sizeof(i2s_config_t));
    i2s_config.drive_mode = drive_mode;
    i2s_config.transfer_mode = transfer_mode;
    i2s_config.data_width = data_width;
    i2s_config.channels_num = channel_num_sio;
    i2s_config.timing = timing;
    i2s_config.clk_edge = clk_edge;
    i2s_config.div_number = div_num;
    i2s_config.number_of_channels = channel_num_true;

    errcode_t ret = uapi_i2s_set_config(i2s_num, &i2s_config);
    if (ret != ERRCODE_SUCC) {
        osal_printk("I2S配置设置失败，错误码：%u\n", ret);
    }

    // SED : 情况
    uapi_i2s_set_sample_rate(i2s_num, sample_rate);
    uapi_i2s_set_crg_clock_enable(i2s_num, true);
}

void pcm2706::i2s_dma_init_2()
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

void pcm2706::sem_mutex_init()
{
    osal_sem_init(&is_data_ready, 0);
    osal_sem_init(&is_buffer_empty, buffer_num); // 初始时所有缓冲区都空
    osal_mutex_init(&dma_mutex);

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

void pcm2706::i2s_read_callback(uint8_t intr, uint8_t channel, uintptr_t arg)
{
    unused(channel);
    unused(arg);

    if (intr == HAL_DMA_INTERRUPT_TFR) {
        osal_dcache_region_inv((void *)dma_buffers[write_idx], buffer_size * sizeof(uint16_t));

        uint32_t new_write_idx = (write_idx + 1) % buffer_num; // 计算下一个写入索引
        if (pending_frames < buffer_num) {
            write_idx = new_write_idx;
            pending_frames++;
            osal_sem_up(&is_data_ready); // 通知数据准备就绪
        } else {
            write_idx = new_write_idx;
            // 强制更新读索引以丢弃最旧数据，保持写索引和读索引的正确关系
            read_idx = (read_idx + 1) % buffer_num;
        }
    }
}

void pcm2706::dma_lli_init()
{
    dma_channel = uapi_dma_get_lli_channel(0, HAL_DMA_HANDSHAKING_MAX_NUM);

    // 确保通道号有效
    if (dma_channel >= DMA_CHANNEL_MAX_NUM) {
        osal_printk("获取LLI通道失败\n");
    }

    dma_ch_user_peripheral_config_t dma_ch_config;
    memset(&dma_ch_config, 0, sizeof(dma_ch_user_peripheral_config_t));
    dma_ch_config.src = (uint32_t)i2s_porting_rx_merge_data_addr_get(i2s_num);
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
        dma_ch_config.dest = (uint32_t)(uintptr_t)dma_buffers[i];
        errcode_t err = uapi_dma_configure_peripheral_transfer_lli(dma_channel, &dma_ch_config, i2s_read_callback);
        if (err != 0) {
            // SED_LOG : DMA LLI配置失败
            osal_printk("DMA LLI配置失败，错误码: %u\n", err);
        }
    }

    osal_flush_cache();

    errcode_t start_ret = uapi_dma_enable_lli(dma_channel, i2s_read_callback, (uintptr_t)nullptr);
    if (start_ret != ERRCODE_SUCC) {
        osal_printk("DMA LLI启动失败，错误码: 0x%x\n", start_ret);
    }

    osal_flush_cache();

    // SED : 情况
    hal_sio_set_crg_clock_enable(i2s_num, true);
    hal_sio_set_rx_enable(i2s_num, 1);
}

