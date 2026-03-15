#include "g711.hpp"

uint8_t *g711_alaw_encoder::encode(const int16_t *indata, int len)
{
    static std::array<uint8_t, 1024> outdata; // 输出缓冲区，大小根据实际需求调整
    
    return outdata.data();
}