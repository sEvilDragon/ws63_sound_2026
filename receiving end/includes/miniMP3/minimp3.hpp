#ifndef __MINIMP3_H__
#define __MINIMP3_H__

extern "C" {
#include "lwip/sockets.h" // 实现网络下载功能
#include "soc_osal.h"
}
#include "http_utils.hpp" // 网络工具包
// 使用开源协议的minimp3库来解码MP3文件
// #define MINIMP3_IMPLEMENTATION // 移至 .cpp 文件中，避免多重定义
#include "miniMP3/minimp3.h"
#include <array>

class minimp3 {
public:
    // 注册实时更改iis频率的函数指针
    using iis_set_rate = void (*)(int rate);
    static void iis_set_rate_set(iis_set_rate set_rate_func);
    // 注册将解码后的PCM数据传入iis的函数指针
    using mp3_get_into_iis = void (*)(const int16_t *data, uint32_t size);
    static void mp3_get_into_iis_set(mp3_get_into_iis get_into_iis_func);
    // 注册播放队列水位读取函数（用于解码侧闭环背压）
    using playback_queue_level_getter = int (*)();
    static void playback_queue_level_getter_set(playback_queue_level_getter getter_func);
    // 仅准备URL，不改变播放开关；用于DLNA SetAVTransportURI。
    static void prepare_url(const char *url);
    static void play_url(const char *url);
    static void stop_playback();
    static void clear_playback_url();
    static void stream_mp3_to_iis();

    // 暂停/恢复/跳转接口（供DLNA层调用）
    static void pause_playback();                  // 真暂停，记录字节偏移以便恢复
    static void resume_playback();                 // 从暂停位置恢复播放
    static bool get_is_paused();                   // 查询当前是否处于暂停状态
    static void seek_to_seconds(uint32_t seconds); // 跳转到指定时间位置
    static uint32_t get_duration_seconds();        // 获取估算的歌曲总时长（秒）

private:
    static void bump_stream_epoch();
    static void http_set_url(const char *url, bool start_playback);
    static void http_get_url(const char *url);
    static void http_stop();
    static void http_clear_url();

    // 定义函数指针
    static iis_set_rate iis_set_rate_func;
    static mp3_get_into_iis mp3_get_into_iis_func;
    static playback_queue_level_getter playback_queue_level_getter_func;

    // 存放当前的HTTP下载URL
    static std::array<char, 512> current_url;

    // 记录状态
    static bool is_playing;
    static bool is_url_ready;
    static bool is_paused;                   // 是否处于暂停状态
    static bool s_has_range;                 // 下次连接是否携带 Range 头
    static bool s_interrupt_stream;          // 请求中断当前内层流循环（用于seek打断）
    static volatile uint32_t s_stream_epoch; // 控制代际，避免旧流在pause/seek后继续喂PCM
    static uint64_t s_range_start_byte;      // Range 起始字节偏移
    static uint64_t s_resume_target_byte;    // 逻辑恢复点，Range 可从更早位置预卷启动
    static uint64_t s_mp3_start_offset;      // HTTP 文件中 MP3 数据起始偏移
    static uint64_t s_content_length;        // HTTP Content-Length（字节，0=未知）
    static uint32_t s_duration_seconds;      // 估算歌曲时长（秒，0=未知）
    static uint64_t s_bytes_streamed;        // 当前播放位置（文件绝对字节偏移）
    static uint32_t s_avg_bitrate_bps;       // 平均比特率（bps，由解码帧统计）

    // 定义解码相关的成员变量
    // 固定20KB输入窗口，使用普通数组，避免动态分配带来的内存碎片和抖动。
    static constexpr size_t k_mp3_buffer_chunk_size = 1024;
    static constexpr size_t k_mp3_buffer_chunk_count = 20;
    static constexpr size_t mp3_buffer_size = k_mp3_buffer_chunk_size * k_mp3_buffer_chunk_count;
};

#endif
