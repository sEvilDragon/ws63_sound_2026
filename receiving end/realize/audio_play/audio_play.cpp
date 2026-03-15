#include "audio_play.hpp"

static void sle_data_process(const uint8_t *data, uint16_t len)
{
    if (data == nullptr || len <= 6)
        return;

    pcm5102::data_write((const int16_t *)data, len / sizeof(int16_t));
    pcm5102::fill_buffer_if_needed();
}

void *audio_play_task(void *arg)
{
    (void)arg;
    static pcm5102 g_pcm5102;
    osal_printk("PCM5102实例已创建\n");

    sle::set_data_process_fuction(sle_data_process);
    sle::set_data_clear_fuction(pcm5102::data_clear);

    static sle g_sle;
    osal_printk("SLE实例已创建\n");
    osal_printk("实例已完全创建\n");

    while (true) {
        osal_msleep(1000);
    }
}