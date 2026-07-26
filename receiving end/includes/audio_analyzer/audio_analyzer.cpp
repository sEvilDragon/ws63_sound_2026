#include "audio_analyzer.hpp"
#include <cmath>

int16_t audio_analyzer::sample_buf[audio_analyzer::BLOCK_SIZE];
volatile uint32_t audio_analyzer::sample_count = 0;
audio_result_t audio_analyzer::result[2] = {};
volatile uint8_t audio_analyzer::result_idx = 0;
volatile bool audio_analyzer::buf_full = false;
int32_t audio_analyzer::energy_history[6] = {};
uint8_t audio_analyzer::energy_hist_idx = 0;
float audio_analyzer::max_band[audio_analyzer::NUM_BANDS] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
float audio_analyzer::max_overall = 1.0f;

void audio_analyzer::add_samples(const int16_t *data, uint32_t size)
{
    if (buf_full)
        return;
    for (uint32_t i = 0; i < size; i += 2) {
        if (sample_count >= BLOCK_SIZE) {
            buf_full = true;
            break;
        }
        sample_buf[sample_count++] = data[i];
    }
}

void audio_analyzer::compute()
{
    if (!buf_full)
        return;

    uint8_t wi = 1 - result_idx;
    float total_energy = 0;
    float band_energy[NUM_BANDS];

    for (int b = 0; b < NUM_BANDS; b++) {
        float coeff = 2.0f * cosf(2.0f * 3.14159265f * TARGET_FREQS[b] / SAMPLE_RATE);

        float s0 = 0, s1 = 0, s2 = 0;
        for (uint32_t i = 0; i < BLOCK_SIZE; i++) {
            s0 = (float)sample_buf[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        float magnitude_sq = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        band_energy[b] = magnitude_sq / ((float)BLOCK_SIZE * BLOCK_SIZE);
        total_energy += band_energy[b];
    }

    for (int b = 0; b < NUM_BANDS; b++) {
        if (band_energy[b] > max_band[b]) {
            max_band[b] = band_energy[b];
        } else {
            max_band[b] *= 0.995f;
        }
        /* 防止 AGC 将静音噪声放大到满幅: max_band 不低于此阈值 */
        if (max_band[b] < 50000.0f) {
            max_band[b] = 50000.0f;
        }

        float normalized = band_energy[b] / max_band[b];
        if (normalized > 1.0f)
            normalized = 1.0f;
        result[wi].bands[b] = (uint8_t)(normalized * normalized * 255.0f);
    }

    if (total_energy > max_overall) {
        max_overall = total_energy;
    } else {
        max_overall *= 0.995f;
    }
    if (max_overall < 50000.0f * NUM_BANDS) {
        max_overall = 50000.0f * NUM_BANDS;
    }

    float norm_overall = total_energy / max_overall;
    if (norm_overall > 1.0f)
        norm_overall = 1.0f;
    result[wi].overall = (uint8_t)(norm_overall * norm_overall * 255.0f);

    int32_t current_energy = (int32_t)total_energy;
    int32_t avg = 0;
    for (int i = 0; i < 6; i++) {
        avg += energy_history[i];
    }
    avg /= 6;

    energy_history[energy_hist_idx] = current_energy;
    energy_hist_idx = (energy_hist_idx + 1) % 6;

    result[wi].beat = (current_energy > avg * 3 / 2 && avg > 0) ? 1 : 0;

    /* 噪声门: 绝对能量低于阈值时强制输出 0, 防止 AGC 把噪声底放大到满幅 */
    if (total_energy < 20000.0f) {
        for (int b = 0; b < NUM_BANDS; b++) {
            result[wi].bands[b] = 0;
        }
        result[wi].overall = 0;
        result[wi].beat = 0;
    }

    /* 每 80 帧打印一次原始能量 (精简版, 避免格式化溢出) */
    {
        static uint32_t dbg_cnt = 0;
        dbg_cnt++;
        if (dbg_cnt % 80 == 0) {
            osal_printk("[RAW] total=%.0f bands=[%.0f %.0f %.0f %.0f %.0f]\r\n", (double)total_energy,
                        (double)band_energy[0], (double)band_energy[1], (double)band_energy[2], (double)band_energy[3],
                        (double)band_energy[4]);
        }
    }

    result_idx = wi;
    sample_count = 0;
    buf_full = false;
}

const audio_result_t &audio_analyzer::get_result()
{
    return result[result_idx];
}

void audio_analyzer::reset()
{
    unsigned long flags = osal_irq_lock();

    sample_count = 0;
    buf_full = false;
    result[0] = {};
    result[1] = {};
    result_idx = 0;
    for (uint8_t i = 0; i < 6; i++) {
        energy_history[i] = 0;
    }
    energy_hist_idx = 0;
    for (uint8_t i = 0; i < NUM_BANDS; i++) {
        max_band[i] = 1.0f;
    }
    max_overall = 1.0f;

    osal_irq_restore(flags);
}
