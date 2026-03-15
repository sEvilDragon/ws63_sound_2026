#ifndef __ADPCM_H__
#define __ADPCM_H__

#include <cstdint>
#include <array>

class adpcm {
public:
    adpcm() {
        reset();
    }
    ~adpcm() = default;

    /**
     * @brief 编码 PCM 数据为 ADPCM (8-bit)
     * @param indata 输入的 PCM16 数据 (左右声道交替 LRLR)
     * @param len 样本数量 (单声道样本数 * 2)
     * @return 指向静态输出缓冲区的指针
     */
    uint8_t *encode(const int16_t *indata, int len);

    /**
     * @brief 解码 ADPCM 数据为 PCM16
     * @param indata 输入的 ADPCM 8-bit 数据，非交织格式：前 len/2 字节为L声道，后 len/2 字节为R声道
     * @param len 总数据长度（必须为偶数）
     * @param outdata 输出的 PCM16 缓冲区，交织格式：[L0][R0][L1][R1]... 共 len 个 int16_t
     */
    void decode(const uint8_t *indata, int len, int16_t *outdata);

    void reset()
    {
        last_prediction[0] = 0;
        last_prediction[1] = 0;
        last_index[0] = 0;
        last_index[1] = 0;
    }

    void set_state(int16_t lv, int8_t li, int16_t rv, int8_t ri)
    {
        last_prediction[0] = lv;
        last_index[0]      = li;
        last_prediction[1] = rv;
        last_index[1]      = ri;
    }

private:
    int16_t last_prediction[2];
    int8_t last_index[2];

    static const std::array<int8_t, 16> index_table;
    static const std::array<uint16_t, 89> stepsize_table;
    static std::array<uint8_t, 2048> out_buffer;
};

#endif