#include "adpcm.hpp"

/* 静态数组定义 (使用 std::array，在现代编译器优化下开销与基础数组一致) */
const std::array<int8_t, 16> adpcm::index_table = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

const std::array<uint16_t, 89> adpcm::stepsize_table = {
    7,    8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,   28,
    31,   34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,  118,
    130,  143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,  494,
    544,  598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878, 2066,
    2272, 2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

std::array<uint8_t, 2048> adpcm::out_buffer = {0};

uint8_t *adpcm::encode(const int16_t *indata, int len)
{
    // len = 960 交错立体声样本数 (480L + 480R)
    int samples_per_channel = len / 2;
    uint8_t *outp = out_buffer.data();

    // 写入 6 字节状态头（编码前的状态，接收端用于流程同步）
    // 格式: [L_val_low, L_val_high, L_index, R_val_low, R_val_high, R_index]
    int16_t lv = last_prediction[0];
    int16_t rv = last_prediction[1];
    *outp++ = (uint8_t)(lv & 0xFF);
    *outp++ = (uint8_t)((lv >> 8) & 0xFF);
    *outp++ = (uint8_t)last_index[0];
    *outp++ = (uint8_t)(rv & 0xFF);
    *outp++ = (uint8_t)((rv >> 8) & 0xFF);
    *outp++ = (uint8_t)last_index[1];

    // 编码 L 声道（输入偏移 0, 2, 4...）
    int32_t valpred_l = last_prediction[0];
    int32_t step_idx_l = last_index[0];
    for (int i = 0; i < samples_per_channel; i++) {
        int32_t val = (int32_t)indata[i * 2];
        int32_t step = (int32_t)stepsize_table[step_idx_l];
        int32_t diff = val - valpred_l;
        uint8_t sign = (diff < 0) ? 0x80 : 0x00;
        if (sign) diff = -diff;
        int32_t magnitude = (diff * 64) / step;
        if (magnitude > 127) magnitude = 127;
        *outp++ = (uint8_t)(sign | magnitude);
        int32_t vpdiff = (magnitude * step) >> 6;
        if (sign) valpred_l -= vpdiff;
        else valpred_l += vpdiff;
        if (valpred_l > 32767) valpred_l = 32767;
        else if (valpred_l < -32768) valpred_l = -32768;
        step_idx_l += index_table[magnitude >> 4];
        if (step_idx_l < 0) step_idx_l = 0;
        else if (step_idx_l > 88) step_idx_l = 88;
    }

    // 编码 R 声道（输入偏移 1, 3, 5...）
    int32_t valpred_r = last_prediction[1];
    int32_t step_idx_r = last_index[1];
    for (int i = 0; i < samples_per_channel; i++) {
        int32_t val = (int32_t)indata[i * 2 + 1];
        int32_t step = (int32_t)stepsize_table[step_idx_r];
        int32_t diff = val - valpred_r;
        uint8_t sign = (diff < 0) ? 0x80 : 0x00;
        if (sign) diff = -diff;
        int32_t magnitude = (diff * 64) / step;
        if (magnitude > 127) magnitude = 127;
        *outp++ = (uint8_t)(sign | magnitude);
        int32_t vpdiff = (magnitude * step) >> 6;
        if (sign) valpred_r -= vpdiff;
        else valpred_r += vpdiff;
        if (valpred_r > 32767) valpred_r = 32767;
        else if (valpred_r < -32768) valpred_r = -32768;
        step_idx_r += index_table[magnitude >> 4];
        if (step_idx_r < 0) step_idx_r = 0;
        else if (step_idx_r > 88) step_idx_r = 88;
    }

    last_prediction[0] = (int16_t)valpred_l;
    last_prediction[1] = (int16_t)valpred_r;
    last_index[0] = (int8_t)step_idx_l;
    last_index[1] = (int8_t)step_idx_r;

    return out_buffer.data();
}

void adpcm::decode(const uint8_t *indata, int len, int16_t *outdata)
{
    // indata格式：非交织，前半段为L声道，后半段为R声道
    // outdata格式：交织，[L0][R0][L1][R1]...（I2S DMA直接消费）
    int samples_per_channel = len / 2;
    const uint8_t *l_data = indata;
    const uint8_t *r_data = indata + samples_per_channel;

    int32_t valpred[2] = {last_prediction[0], last_prediction[1]};
    int32_t step_idx[2] = {last_index[0], last_index[1]};

    for (int i = 0; i < samples_per_channel; i++) {
        // 解码L声道
        {
            uint8_t delta = l_data[i];
            int32_t step = (int32_t)stepsize_table[step_idx[0]];
            uint8_t sign = delta & 0x80;
            int32_t magnitude = delta & 0x7F;
            step_idx[0] += index_table[magnitude >> 4];
            if (step_idx[0] < 0) step_idx[0] = 0;
            else if (step_idx[0] > 88) step_idx[0] = 88;
            int32_t vpdiff = (magnitude * step) >> 6;
            if (sign) valpred[0] -= vpdiff;
            else      valpred[0] += vpdiff;
            if (valpred[0] > 32767)       valpred[0] = 32767;
            else if (valpred[0] < -32768) valpred[0] = -32768;
            outdata[i * 2] = (int16_t)valpred[0];
        }
        // 解码R声道
        {
            uint8_t delta = r_data[i];
            int32_t step = (int32_t)stepsize_table[step_idx[1]];
            uint8_t sign = delta & 0x80;
            int32_t magnitude = delta & 0x7F;
            step_idx[1] += index_table[magnitude >> 4];
            if (step_idx[1] < 0) step_idx[1] = 0;
            else if (step_idx[1] > 88) step_idx[1] = 88;
            int32_t vpdiff = (magnitude * step) >> 6;
            if (sign) valpred[1] -= vpdiff;
            else      valpred[1] += vpdiff;
            if (valpred[1] > 32767)       valpred[1] = 32767;
            else if (valpred[1] < -32768) valpred[1] = -32768;
            outdata[i * 2 + 1] = (int16_t)valpred[1];
        }
    }

    last_prediction[0] = (int16_t)valpred[0];
    last_prediction[1] = (int16_t)valpred[1];
    last_index[0] = (int8_t)step_idx[0];
    last_index[1] = (int8_t)step_idx[1];
}