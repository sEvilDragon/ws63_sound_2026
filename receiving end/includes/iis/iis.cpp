#include "iis.hpp"
#include "audio_analyzer.hpp"
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
    0,     110,   116,   123,   130,   138,   146,   155,   164,   174,   184,   195,   207,   219,   232,
    246,   260,   276,   292,   309,   328,   347,   368,   389,   413,   437,   463,   490,   519,   550,
    583,   617,   654,   693,   734,   777,   823,   872,   923,   978,   1036,  1098,  1163,  1232,  1304,
    1382,  1464,  1550,  1642,  1740,  1843,  1952,  2067,  2190,  2320,  2457,  2603,  2757,  2920,  3093,
    3277,  3471,  3677,  3894,  4125,  4370,  4628,  4903,  5193,  5501,  5827,  6172,  6538,  6925,  7336,
    7770,  8231,  8718,  9235,  9782,  10362, 10976, 11626, 12315, 13045, 13818, 14636, 15504, 16422, 17395,
    18426, 19518, 20675, 21900, 23197, 24572, 26028, 27570, 29204, 30934, 32767};

iis::iis()
{
    osal_msleep(4000); // �ȴ�ϵͳ�ȶ���ȷ��DMA��I2S����׼������
    pin_init();
    i2s_dma_init_1();
    i2s_init();
    i2s_dma_init_2();
    set_rate_of_iis(i2s_sample_rate);
    sem_mutex_init();
    dma_lli_init();
    osal_printk("[IIS] init complete, buffers=%d, TX initially off\r\n", buffer_num);
}

iis::~iis()
{
    hal_sio_set_tx_enable(i2s_num, 0);
    for (int i = 0; i < (int)buffer_num; i++) {
        if (raw_buffers[i] != nullptr) {
            osal_kfree(raw_buffers[i]);
            raw_buffers[i] = nullptr;
            dma_buffers[i] = nullptr;
        }
    }
}

void iis::pin_init()
{
    // �������Ź���
    uapi_pin_set_mode(mcsl_pin, PIN_MODE_4);
    uapi_pin_set_mode(sclk_pin, PIN_MODE_4);
    uapi_pin_set_mode(lrclk_pin, PIN_MODE_4);
    uapi_pin_set_mode(dataout_pin, PIN_MODE_4);

    // �������ŷ���
    uapi_pin_set_ds(mcsl_pin, PIN_DS_4);
    uapi_pin_set_ds(sclk_pin, PIN_DS_4);
    uapi_pin_set_ds(lrclk_pin, PIN_DS_4);
    uapi_pin_set_ds(dataout_pin, PIN_DS_4);

    // LRCLK/DIN���Źٷ���ֲ��ر������������ﱣ��һ��
    uapi_pin_set_pull(lrclk_pin, PIN_PULL_TYPE_DISABLE);
    uapi_pin_set_pull(dataout_pin, PIN_PULL_TYPE_DISABLE);
}

void iis::i2s_dma_init_1()
{
    /* uapi_dma_init/open 已在 app_entry 中统一调用, 此处无需重复 */
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
        osal_printk("I2S��������ʧ��1�������룺%u\n", ret1);
    }

    // ʹ��I2Sʱ�ӣ��ȿ�ʱ�ӣ�����DMA��
    uapi_i2s_set_crg_clock_enable(i2s_num, true);
    osal_msleep(10);
}

void iis::set_rate_of_iis(i2s_sample_rate_t rate)
{
    errcode_t ret = uapi_i2s_set_sample_rate(i2s_num, rate);
    if (ret != ERRCODE_SUCC) {
        osal_printk("I2S����������ʧ�ܣ������룺%u\n", ret);
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
        osal_printk("I2S DMA��������ʧ�ܣ������룺%u\n", ret);
    }
}

void iis::sem_mutex_init()
{
    raw_buffers.fill(nullptr);
    dma_buffers.fill(nullptr);

    // ��˼�漰cache��������Ҫ�ֶ�����Cache Line
    for (int i = 0; i < buffer_num; i++) {
        uint32_t buf_size = buffer_size * sizeof(uint16_t) + cache_size; // ����ռ����ڶ���
        raw_buffers[i] = osal_kmalloc(buf_size, OSAL_GFP_DMA);

        // ����ʧ���������ѷ������Դ������
        if (raw_buffers[i] == nullptr) {
            for (int j = 0; j < i; j++) {
                osal_kfree(raw_buffers[j]);
                raw_buffers[j] = nullptr;
                dma_buffers[j] = nullptr;
            }
            return;
        }

        // �ֶ������ڴ��ַ��Cache Line��С
        uintptr_t addr = (uintptr_t)raw_buffers[i];
        // ���뵽��һ��cache line�߽�
        // ����ķ���ʵ����ȥ�������е�cache_sizeλ�ĵ�ַ���Ӷ�ʵ�������¶��룻����cache_size - 1��ʵ�������϶���
        uintptr_t aligned_addr = (addr + cache_size - 1) & ~(cache_size - 1);

        dma_buffers[i] = (uint16_t *)aligned_addr;
        for (int j = 0; j < buffer_size; j++) {
            dma_buffers[i][j] = 0; // ��ʼ������������Ϊ0
        }
    }
    osal_printk("[IIS] DMA buffers allocated: %d x %d samples\r\n", buffer_num, buffer_size);
}

void iis::i2s_send_callback(uint8_t intr, uint8_t channel, uintptr_t arg)
{
    unused(channel);
    unused(arg);

    if (intr == HAL_DMA_INTERRUPT_TFR) {

        int old_read_idx = read_idx;
        uint32_t new_read_idx = (read_idx + 1) % buffer_num;

        // �������շ�����Ļ�������ֻ���� DMA
        // ����ɲۣ��޾�̬��
        data_clear_one(old_read_idx);

        // �ٽ��������� pending_frames / read_idx �� data_write �Ĳ���
        uint32_t irq = osal_irq_lock();
        if (pending_frames > 0)
            pending_frames--;
        read_idx = new_read_idx;
        osal_irq_restore(irq);

        // ÿ 6000 �λص���ӡһ�Σ���2���ӣ�������ˢ��
        static int cb_count = 0;
        cb_count++;
        if (cb_count % 6000 == 1) {
            // �� write/read ��ֵʵʱ���� pending�����������Ư����
            int real_pending = (write_idx - (int)read_idx + (int)buffer_num) % (int)buffer_num;
            osal_printk("[IIS] DMA cb #%d, read=%d, write=%d, pending=%d\r\n", cb_count, read_idx, write_idx,
                        real_pending);
        }

        // Ƿ�ؼ�⣺�ü򵥼���բ�ţ����� was_underrun �񵴵���ˢ��
        static int underrun_silence = 0;
        if (is_ready && pending_frames <= min_buffer_num) {
            if (underrun_silence <= 0) {
                osal_printk("[IIS] underrun, pending=%d, read=%d (TX stays on)\r\n", pending_frames, read_idx);
                underrun_silence = 3000; // ���ƽ����� 3000 �λص� (~1����)
            }
        } else {
            underrun_silence = 0; // �ָ��������´�Ƿ����������
        }
        if (underrun_silence > 0)
            underrun_silence--;
    }
}

void iis::dma_lli_init()
{
    dma_channel = uapi_dma_get_lli_channel(0, HAL_DMA_HANDSHAKING_MAX_NUM);

    // ȷ��ͨ������Ч
    if (dma_channel >= DMA_CHANNEL_MAX_NUM) {
        osal_printk("��ȡLLIͨ��ʧ��\n");
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
            // SED_LOG : DMA LLI����ʧ��
            osal_printk("DMA LLI����ʧ�ܣ�������: %u\n", err);
        }
    }

    osal_flush_cache();

    errcode_t start_ret = uapi_dma_enable_lli(dma_channel, i2s_send_callback, (uintptr_t)nullptr);
    if (start_ret != ERRCODE_SUCC) {
        osal_printk("DMA LLI����ʧ�ܣ�������: 0x%x\n", start_ret);
    }

    osal_flush_cache();

    // SED : ���
    hal_sio_set_crg_clock_enable(i2s_num, true);
    // TX ����������ȴ� data_write
    // �����㹻֡�����ٿ��������⻺��������ʱ��������
    hal_sio_set_tx_enable(i2s_num, 0);
    osal_printk("[IIS] DMA LLI started, ch=%d, buffers=%d, TX initially off\r\n", dma_channel, buffer_num);
}

void iis::data_write(const int16_t *data, uint32_t size, uint8_t volume, uint8_t bass)
{
    static int dw_count = 0;
    if (++dw_count % 200 == 1) {
        osal_printk("[IIS] data_write #%d, size=%u, pending=%d, write_idx=%d, offset=%d, ready=%d\r\n", dw_count, size,
                    pending_frames, write_idx, write_offset, is_ready);
    }

    if (size % 2 != 0) {
        size--;
    }

    audio_analyzer::add_samples(data, size);

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
            static int drop_count = 0;
            if (++drop_count % 50 == 1) {
                osal_printk("[IIS] buffer full, dropping data (x%d)\r\n", drop_count);
            }
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
                float y = bass_b0 * x + bass_b1 * biquad_bx1_l + bass_b2 * biquad_bx2_l - bass_a1 * biquad_by1_l -
                          bass_a2 * biquad_by2_l;
                biquad_bx2_l = biquad_bx1_l;
                biquad_bx1_l = x;
                biquad_by2_l = biquad_by1_l;
                biquad_by1_l = y;
                if (y > 32767.0f)
                    y = 32767.0f;
                else if (y < -32768.0f)
                    y = -32768.0f;
                dst[i] = (int16_t)y;
                if (i + 1 < to_copy) {
                    x = (float)dst[i + 1];
                    y = bass_b0 * x + bass_b1 * biquad_bx1_r + bass_b2 * biquad_bx2_r - bass_a1 * biquad_by1_r -
                        bass_a2 * biquad_by2_r;
                    biquad_bx2_r = biquad_bx1_r;
                    biquad_bx1_r = x;
                    biquad_by2_r = biquad_by1_r;
                    biquad_by1_r = y;
                    if (y > 32767.0f)
                        y = 32767.0f;
                    else if (y < -32768.0f)
                        y = -32768.0f;
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
                osal_printk("[IIS] TX ENABLED, pending_frames=%d, write_idx=%d\r\n", pending_frames, write_idx);
            }
            osal_irq_restore(irq);
        }
    }
}

void iis::data_clear()
{
    osal_printk("[IIS] data_clear: pending=%d, read=%d, write=%d\r\n", pending_frames, read_idx, write_idx);
    // ���ٹر� TX��DMA LLI һ���������������У��� TX ֻ��������ţ�
    // DMA �ڲ������Լ������Ļ����������� pending_frames
    // ��Զ�޷����»��ۣ� TX ����ʧȥ�ؿ����ᡣ��Ϊ���� TX �������� DMA ����������ľ���֡��

    for (int i = 0; i < buffer_num; i++) {
        memset(dma_buffers[i], 0, buffer_size * sizeof(uint16_t));
        // DMA ֱ�Ӷ������ڴ棬���� writeback ���� DMA �Զ�������Ƶ����
        osal_dcache_region_clean(dma_buffers[i], buffer_size * sizeof(uint16_t));
    }
    // ���ܽ� read_idx ���㣺DMA LLI Ӳ���ڵ㲻�����������ö����㡣
    // ��ǿ�� read_idx=0 �� DMA ͣ�ڽڵ� N�������� DMA �� N ������ֵ�ۣ�
    // ÿ��һ�ۻص��ݼ� pending_frames���ڵ���������ǰ�ʹ��� TX �ر� �� ���þ�����
    // ��ȷ���������� read_idx ��Ӳ��ͬ����write_idx ���뵽 read_idx��
    // �����ݴ� DMA ��ǰλ�������룬pending_frames ��ʵ�ʿ��ò�һһ��Ӧ��
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
        return; // ����Խ�磬ֱ�ӷ���
    }
    // Ƿ��ʱ��������ֵ��䣬�ϴ����������ײ���ͻأ������
    for (uint32_t i = 0; i + 1 < buffer_size; i += 2) {
        dma_buffers[index][i] = last_left_sample;
        dma_buffers[index][i + 1] = last_right_sample;
    }
    // DMA ֱ�Ӷ������ڴ棬memset ֻд CPU cache���� writeback �� DMA ���������ڴ���ľ���Ƶ��
    // ����ÿ����ȫ��ͬ���ظ�������
    osal_dcache_region_clean(dma_buffers[index], buffer_size * sizeof(uint16_t));
}

void iis::fill_buffer_if_needed()
{
    if (pending_frames < if_fill_num) {
        // �ظ�һ��֡�������ⶶ��
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
        // ������֡����ʱ�������������ݣ������ѹ����֡���³�ʱ����ӳ�
        uint32_t drop_size = if_small_num * 2; // ÿ�ζ��� if_small_num ֡
        if (size > drop_size) {
            size -= drop_size;
        }
    }
}
