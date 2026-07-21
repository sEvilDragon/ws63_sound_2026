#include "audio_read_send.h"

#include "../../../other/adpcm/adpcm.hpp"

#include <cstring>

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

    /* ADPCM只在SLE发送前使用，PCM2706采集数据本身始终保持原始PCM。 */
    static adpcm g_adpcm;
    static uint8_t adpcm_packet[sle_audio::adpcm_encoded_size(pcm2706::buffer_size)] = {0};
    static constexpr int chunk_samples = 360;
    static uint8_t pcm_packet[1 + chunk_samples * sizeof(int16_t)] = {0};
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

        /*
         * 只在即将写入SLE时选择编码。ADPCM整帧约490字节，一次write即可；
         * 原始PCM仍分包，但每包也增加一个非0xF标志字节。
         */
        const bool use_adpcm = sle::compression_enabled();
        std::size_t adpcm_length = 0;
        if (use_adpcm) {
            adpcm_length = g_adpcm.encode(data_ptr, pcm.buffer_size, adpcm_packet, sizeof(adpcm_packet));
            if (adpcm_length == 0) {
                osal_printk("[Audio] ADPCM encode failed, dropping frame\r\n");
                continue;
            }
        }

        for (int i = 0; i < sle::max_connection_num; i++) {
            if (sle::connection_devices[i].is_active && sle::connection_devices[i].target_handle != 0) {
                if (use_adpcm) {
                    sle::write_send(i, adpcm_packet, static_cast<uint16_t>(adpcm_length));
                    continue;
                }

                for (int offset = 0; offset < pcm.buffer_size; offset += chunk_samples) {
                    int send_samples = pcm.buffer_size - offset;
                    if (send_samples > chunk_samples)
                        send_samples = chunk_samples;
                    const std::size_t payload_bytes = send_samples * sizeof(int16_t);
                    pcm_packet[0] = sle_audio::pcm_packet_marker;
                    std::memcpy(pcm_packet + 1, data_ptr + offset, payload_bytes);
                    sle::write_send(i, pcm_packet, static_cast<uint16_t>(payload_bytes + 1));
                }
            }
        }
    }
}
