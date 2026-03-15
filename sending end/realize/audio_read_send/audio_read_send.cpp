#include "audio_read_send.h"

// 创建pcm2706任务，实现读取音频数据
void *audio_read_send_task(void *arg)
{
    unused(arg);

    // 初始化星闪
    static sle g_sle;
    osal_printk("SLE实例创建完成\n");

    // 创建实例初始化pcm2706
    static pcm2706 pcm;
    osal_printk("PCM2706实例创建完成\n");

    // 初始化去噪算法
    static denoise g_denoise;
    osal_printk("去噪实例创建完成\n");

    // 初始化ADPCM编码器（已禁用，当前使用无压缩原始PCM传输）
    // static adpcm g_adpcm;
    // osal_printk("ADPCM实例创建完成\n");
    osal_printk("实例创建完毕\n");

    while (true) {
        // 等待数据信号量
        osal_sem_down(&pcm.is_data_ready);
        // 定义CPU停止
        uint32_t stop_cpu = osal_irq_lock();
        // 如果当前没有待处理的帧，继续等待
        if (pcm.pending_frames == 0) {
            osal_irq_restore(stop_cpu);
            continue;
        }
        // 说明处理
        int cur_idx = pcm.read_idx;
        pcm.read_idx = (pcm.read_idx + 1) % pcm.buffer_num;
        pcm.pending_frames--;
        osal_irq_restore(stop_cpu);

        // 积压保护：若队列里还有超过阈值的旧帧，直接跳过，避免突发大量数据给接收端
        // 接收端 Watermark High 由突发包引发，根源是 SLE 短暂中断后积压帧一次性涌入
        // {
        //     const int MAX_BACKLOG = 4;
        //     uint32_t s = osal_irq_lock();
        //     if (pcm.pending_frames > MAX_BACKLOG) {
        //         int drop = pcm.pending_frames - MAX_BACKLOG;
        //         pcm.read_idx = (pcm.read_idx + drop) % pcm.buffer_num;
        //         pcm.pending_frames -= drop;
        //     }
        //     osal_irq_restore(s);
        // }

        // 获取当前缓冲区的数据指针
        int16_t *data_ptr = pcm.dma_buffers[cur_idx];

        // 除杂
        // g_denoise.process(data_ptr, pcm.buffer_size);

        // 星闪发送（无压缩原始PCM）
        // MTU = 1000 字节，int16_t 占 2 字节，每包最多 360 个样本（1400 字节 < 1500）
        // buffer_size=960 正好 1.47 包，比原来 4 包减少一半协议开销和调度次数
        const int chunk_samples = 360;
        for (int i = 0; i < sle::max_connection_num; i++) {
            if (sle::connection_devices[i].is_active && sle::connection_devices[i].target_handle != 0) {
                for (int offset = 0; offset < pcm.buffer_size; offset += chunk_samples) {
                    int send_samples = pcm.buffer_size - offset;
                    if (send_samples > chunk_samples)
                        send_samples = chunk_samples;
                    sle::write_send(i, (uint8_t *)(data_ptr + offset), (uint16_t)(send_samples * (int)sizeof(int16_t)));
                }
            }
        }
    }
}