#pragma once

#include <stdint.h>

extern "C" {
#include "soc_osal.h"
}

struct audio_result_t {
    uint8_t bands[5];   // 5 频带能量 0-255: 60Hz / 250Hz / 1kHz / 4kHz / 10kHz
    uint8_t overall;    // 总体音量 0-255
    uint8_t beat;       // 节拍标志: 1=检测到节拍, 0=无
};

class audio_analyzer {
public:
    static void add_samples(const int16_t *data, uint32_t size);
    static void compute();
    static const audio_result_t &get_result();

    static constexpr uint8_t NUM_BANDS = 5;
    static constexpr uint32_t BLOCK_SIZE = 512;

private:
    static int16_t sample_buf[BLOCK_SIZE];
    static volatile uint32_t sample_count;
    static volatile bool buf_full;
    static audio_result_t result[2];
    static volatile uint8_t result_idx;

    static constexpr float TARGET_FREQS[NUM_BANDS] = {
        60.0f, 250.0f, 1000.0f, 4000.0f, 10000.0f
    };

    static constexpr float SAMPLE_RATE = 44100.0f;

    static int32_t energy_history[6];
    static uint8_t energy_hist_idx;

    static float max_band[NUM_BANDS];
    static float max_overall;
};
