#include "wifi_task.hpp"

namespace {
// 全新闭环：在wifi_task层直接控制解码推进节奏，不再依赖minimp3内部队列背压。
static constexpr int k_queue_hard_high = static_cast<int>(iis::buffer_num) - 6;
static constexpr int k_queue_soft_high = static_cast<int>(iis::buffer_num) - 10;
static constexpr int k_queue_target_low = 6;
static constexpr int k_queue_emergency_low = 2;
static constexpr int k_wait_slice_ms = 2;
static constexpr int k_max_wait_loops = 160;
static bool g_minimp3_uri_ready = false;
static sed_ws63::playback_bridge g_dlna_bridge;
static sed_ws63::renderer g_dlna_renderer(g_dlna_bridge);

errcode_t dlna_set_uri_adapter(const char *uri, const char *metadata)
{
    unused(metadata);
    if (uri == nullptr || uri[0] == '\0') {
        return ERRCODE_FAIL;
    }

    minimp3::prepare_url(uri);
    g_minimp3_uri_ready = true;
    return ERRCODE_SUCC;
}

errcode_t dlna_play_adapter()
{
    if (!g_minimp3_uri_ready) {
        return ERRCODE_FAIL;
    }

    minimp3::play_url(nullptr);
    return ERRCODE_SUCC;
}

errcode_t dlna_pause_adapter()
{
    minimp3::stop_playback();
    return ERRCODE_SUCC;
}

errcode_t dlna_stop_adapter()
{
    minimp3::stop_playback();
    return ERRCODE_SUCC;
}

void push_pcm_with_closed_loop(const int16_t *data, uint32_t size)
{
    if (data == nullptr || size == 0) {
        return;
    }

    int queue_level = static_cast<int>(iis::pending_frames);

    // 高水位节流：队列过高时先等待DMA消耗，避免持续堆积触发后续大幅丢帧/抖动。
    int wait_loops = 0;
    while (queue_level >= k_queue_hard_high && wait_loops < k_max_wait_loops) {
        osal_msleep(k_wait_slice_ms);
        ++wait_loops;
        queue_level = static_cast<int>(iis::pending_frames);
    }

    // 软高水位轻节流：给DMA一点追赶时间，降低队列锯齿。
    if (queue_level >= k_queue_soft_high) {
        osal_msleep(1);
    }

    iis::data_write(data, size);

    // 低水位补偿：接近见底时插入少量保持帧，避免连续欠载造成静音缝隙。
    queue_level = static_cast<int>(iis::pending_frames);
    if (queue_level <= k_queue_emergency_low) {
        iis::fill_buffer_if_needed();
    } else if (queue_level <= k_queue_target_low) {
        iis::fill_buffer_if_needed();
    }
}
} // namespace

void *wifi_task(void *arg)
{
    unused(arg);

    sed_ws63::sta sta_;
    while (wifi_is_wifi_inited() == 0) {
        osal_printk("fail");
        osal_msleep(100);
    }

    sta_.enable_auto_reconnect();

    sed_ws63::softapconfig ap_config;
    sed_ws63::softap_provisioner provisioner;

    while (provisioner.run(sta_, ap_config) != ERRCODE_SUCC) {
        osal_printk("[App] 配网失败，1秒后重试...\n");
        osal_msleep(1000);
    }

    g_dlna_bridge.set_uri_handler_set(dlna_set_uri_adapter);
    g_dlna_bridge.play_handler_set(dlna_play_adapter);
    g_dlna_bridge.pause_handler_set(dlna_pause_adapter);
    g_dlna_bridge.stop_handler_set(dlna_stop_adapter);

    osal_printk("wifi任务启动，启动新的dlna renderer\n");

    g_dlna_renderer.run();
    return NULL;
}

void minimp3_task(void *arg)
{
    unused(arg);
    minimp3 minimp3_;
    iis iis_;
    // 注册iis的回调函数
    minimp3::iis_set_rate_set([](int rate) {
        if (rate == 48000) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_48K);
        } else if (rate == 44100) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_44K);
        } else if (rate == 32000) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_32K);
        } else if (rate == 24000) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_24K);
        } else if (rate == 22050) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_22K);
        } else if (rate == 16000) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_16K);
        } else if (rate == 12000) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_12K);
        } else if (rate == 11025) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_11K);
        } else if (rate == 8000) {
            iis::set_rate_of_iis(I2S_SAMPLE_RATE_8K);
        } else {
            osal_printk("不支持的采样率: %d\n", rate);
        }
    });
    minimp3::mp3_get_into_iis_set([](const int16_t *data, uint32_t size) { push_pcm_with_closed_loop(data, size); });

    // 启用解码侧队列水位反馈，避免解码超速喂数触发IIS端主动丢样本（听感会变成快进）。
    minimp3::playback_queue_level_getter_set([]() -> int { return static_cast<int>(iis::pending_frames); });

    minimp3::stream_mp3_to_iis();
}