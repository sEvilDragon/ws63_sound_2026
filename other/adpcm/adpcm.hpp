#ifndef __ADPCM_H__
#define __ADPCM_H__

#include <cstddef>
#include <cstdint>

namespace sle_audio {

/* Every SLE audio write starts with one packet marker byte. */
constexpr uint8_t packet_type_mask = 0xF0;
constexpr uint8_t pcm_packet_marker = 0x01;
constexpr uint8_t adpcm_packet_marker = 0xF1;

/* Receiver -> sender notification payload. */
constexpr uint8_t codec_control_magic = 0xAC;
constexpr uint8_t codec_control_version = 0x01;
constexpr std::size_t codec_control_size = 3;

/*
 * ADPCM packet layout:
 *   0      : 0xF1 (high nibble 0xF means ADPCM, low nibble is version)
 *   1..2   : decoded interleaved int16 sample count, little-endian
 *   3..6   : left predictor(int16 LE), index(uint8), reserved
 *   7..10  : right predictor(int16 LE), index(uint8), reserved
 *   11..   : one byte per following stereo frame: L nibble low, R nibble high
 *
 * Each packet carries its own IMA state. A lost SLE packet therefore cannot
 * desynchronise all subsequent audio.
 */
constexpr std::size_t adpcm_packet_header_size = 11;

constexpr bool is_adpcm_packet(uint8_t marker)
{
    return (marker & packet_type_mask) == packet_type_mask;
}

constexpr std::size_t adpcm_encoded_size(std::size_t interleaved_samples)
{
    return (interleaved_samples >= 2 && (interleaved_samples & 1U) == 0)
               ? adpcm_packet_header_size + (interleaved_samples / 2U) - 1U
               : 0U;
}

} // namespace sle_audio

/* Standard 4-bit IMA ADPCM codec for interleaved stereo PCM16 packets. */
class adpcm {
public:
    adpcm() { reset(); }
    ~adpcm() = default;

    /* Returns the encoded packet length, or 0 if the arguments are invalid. */
    std::size_t encode(const int16_t *pcm,
                       std::size_t interleaved_samples,
                       uint8_t *output,
                       std::size_t output_capacity);

    /* Returns the decoded interleaved int16 sample count, or 0 on failure. */
    std::size_t decode(const uint8_t *packet,
                       std::size_t packet_length,
                       int16_t *output,
                       std::size_t output_capacity) const;

    void reset();

private:
    int index_[2];

    static uint8_t encode_nibble(int16_t sample, int32_t &predictor, int &index);
    static int16_t decode_nibble(uint8_t nibble, int32_t &predictor, int &index);
};

#endif
