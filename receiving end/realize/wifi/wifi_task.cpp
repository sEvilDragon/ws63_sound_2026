#include "wifi_task.hpp"

void *wifi_task(void *arg)
{
    unused(arg);

    wifi wifi_;
    dlan dlan_;
    // 注册dlan的回调函数
    dlan_.register_media_set_uri_handler(minimp3::play_url);
    dlan_.register_media_play_handler([](const char *uri) -> bool {
        if (uri == nullptr || uri[0] == '\0') {
            return false;
        }
        minimp3::play_url(uri);
        return true;
    });
    dlan_.register_media_pause_handler(minimp3::stop_playback);
    dlan_.register_media_stop_handler(minimp3::stop_playback);

    osal_printk("wifi任务启动，等待网络就绪后启动dlan\n");
    while (true) {
        if (wifi_.is_ready) {
            dlan_.is_ready_set(true);
            osal_printk("wifi已就绪，启动dlan扫描\n");
            break;
        }
        osal_msleep(1000);
    }

    dlan_.ssdp_and_http_scan();
    return NULL;
}

void minimp3_task(void *arg)
{
    unused(arg);
    minimp3 minimp3_;
    iis iis_;
    // 注册iis的回调函数
    minimp3::iis_set_rate_set([](int rate) {
        if (rate == 48000)
        {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_48K);
        } else if (rate == 44100) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_44K);
        } else if (rate == 32000) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_32K);
        } else if (rate == 16000) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_16K);
        } else if (rate == 8000) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_8K);
        } else {
            osal_printk("不支持的采样率: %d\n", rate);
        }
    });
    minimp3::mp3_get_into_iis_set([](const int16_t *data, uint32_t size) {
        iis::data_write(data, size);
        // 仅在队列接近见底时补帧，降低补帧对正常音频连续性的干预。
        if (iis::pending_frames <= 1) {
            iis::fill_buffer_if_needed();
        }
    });
    minimp3::playback_queue_level_getter_set([]() -> int {
        return static_cast<int>(iis::pending_frames);
    });

    minimp3::stream_mp3_to_iis();
}