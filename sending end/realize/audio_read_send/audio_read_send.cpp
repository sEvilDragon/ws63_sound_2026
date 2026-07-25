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
    static constexpr std::size_t mono_frame_samples = pcm2706::buffer_size / 2U;
    static int16_t mono_frame[mono_frame_samples] = {0};
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
        const bool use_mono = sle::mono_enabled();
        static bool mode_logged = false;
        static bool last_adpcm = false;
        static bool last_mono = false;
        if (!mode_logged || use_adpcm != last_adpcm || use_mono != last_mono) {
            osal_printk("[Audio] SLE TX format: %s %s\r\n", use_adpcm ? "ADPCM" : "PCM",
                        use_mono ? "mono" : "stereo");
            mode_logged = true;
            last_adpcm = use_adpcm;
            last_mono = use_mono;
        }
        if (use_mono) {
            for (std::size_t frame = 0; frame < mono_frame_samples; ++frame) {
                const int32_t mixed = static_cast<int32_t>(data_ptr[2U * frame]) + data_ptr[2U * frame + 1U];
                mono_frame[frame] = static_cast<int16_t>(mixed / 2);
            }
        }
        std::size_t adpcm_length = 0;
        if (use_adpcm) {
            adpcm_length = use_mono
                               ? g_adpcm.encode_mono(mono_frame, mono_frame_samples, adpcm_packet,
                                                     sizeof(adpcm_packet))
                               : g_adpcm.encode(data_ptr, pcm.buffer_size, adpcm_packet, sizeof(adpcm_packet));
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

                const int16_t *send_data = use_mono ? mono_frame : data_ptr;
                const int total_samples = use_mono ? static_cast<int>(mono_frame_samples)
                                                   : static_cast<int>(pcm.buffer_size);
                for (int offset = 0; offset < total_samples; offset += chunk_samples) {
                    int send_samples = total_samples - offset;
                    if (send_samples > chunk_samples)
                        send_samples = chunk_samples;
                    const std::size_t payload_bytes = send_samples * sizeof(int16_t);
                    pcm_packet[0] = use_mono ? sle_audio::pcm_mono_packet_marker : sle_audio::pcm_packet_marker;
                    std::memcpy(pcm_packet + 1, send_data + offset, payload_bytes);
                    sle::write_send(i, pcm_packet, static_cast<uint16_t>(payload_bytes + 1));
                }
            }
        }
    }
}
