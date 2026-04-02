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

private:
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

    // 定义解码相关的成员变量
    static constexpr size_t mp3_buffer_size = 4098; // 4KB的MP3数据缓冲区
};

#endif