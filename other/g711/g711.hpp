#ifndef __G711_HPP__
#define __G711_HPP__

#include <array>
#include <cstdint>

class g711_alaw_encoder {
public:
    static uint8_t *encode(const int16_t *indata, int len);

private:
public:
private:
};

#endif