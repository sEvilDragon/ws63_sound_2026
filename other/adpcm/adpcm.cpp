#include "adpcm.hpp"

namespace {

constexpr int8_t kIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

constexpr int16_t kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

constexpr int clamp_index(int index)
{
    return index < 0 ? 0 : (index > 88 ? 88 : index);
}

constexpr int16_t clamp_sample(int32_t sample)
{
    return static_cast<int16_t>(sample < -32768 ? -32768 : (sample > 32767 ? 32767 : sample));
}

void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = static_cast<uint8_t>(value & 0xFFU);
    dst[1] = static_cast<uint8_t>(value >> 8U);
}

uint16_t get_u16_le(const uint8_t *src)
{
    return static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8U);
}

} // namespace

void adpcm::reset()
{
    index_[0] = 0;
    index_[1] = 0;
}

uint8_t adpcm::encode_nibble(int16_t sample, int32_t &predictor, int &index)
{
    const int step = kStepTable[index];
    int diff = static_cast<int>(sample) - predictor;
    uint8_t nibble = 0;
    if (diff < 0) {
        nibble = 0x08;
        diff = -diff;
    }

    int difference = step >> 3;
    int remainder = diff;
    if (remainder >= step) {
        nibble |= 0x04;
        remainder -= step;
        difference += step;
    }
    if (remainder >= (step >> 1)) {
        nibble |= 0x02;
        remainder -= step >> 1;
        difference += step >> 1;
    }
    if (remainder >= (step >> 2)) {
        nibble |= 0x01;
        difference += step >> 2;
    }

    predictor += (nibble & 0x08U) ? -difference : difference;
    predictor = clamp_sample(predictor);
    index = clamp_index(index + kIndexTable[nibble]);
    return nibble;
}

int16_t adpcm::decode_nibble(uint8_t nibble, int32_t &predictor, int &index)
{
    nibble &= 0x0FU;
    const int step = kStepTable[index];
    int difference = step >> 3;
    if (nibble & 0x04U) {
        difference += step;
    }
    if (nibble & 0x02U) {
        difference += step >> 1;
    }
    if (nibble & 0x01U) {
        difference += step >> 2;
    }

    predictor += (nibble & 0x08U) ? -difference : difference;
    predictor = clamp_sample(predictor);
    index = clamp_index(index + kIndexTable[nibble]);
    return static_cast<int16_t>(predictor);
}

std::size_t adpcm::encode(const int16_t *pcm,
                          std::size_t interleaved_samples,
                          uint8_t *output,
                          std::size_t output_capacity)
{
    const std::size_t encoded_size = sle_audio::adpcm_encoded_size(interleaved_samples);
    if (pcm == nullptr || output == nullptr || encoded_size == 0 || encoded_size > output_capacity ||
        interleaved_samples > 0xFFFFU) {
        return 0;
    }

    const std::size_t frames = interleaved_samples / 2U;
    output[0] = sle_audio::adpcm_packet_marker;
    put_u16_le(output + 1, static_cast<uint16_t>(interleaved_samples));

    int32_t predictor[2] = {pcm[0], pcm[1]};
    put_u16_le(output + 3, static_cast<uint16_t>(pcm[0]));
    output[5] = static_cast<uint8_t>(index_[0]);
    output[6] = 0;
    put_u16_le(output + 7, static_cast<uint16_t>(pcm[1]));
    output[9] = static_cast<uint8_t>(index_[1]);
    output[10] = 0;

    uint8_t *payload = output + sle_audio::adpcm_packet_header_size;
    for (std::size_t frame = 1; frame < frames; ++frame) {
        const uint8_t left = encode_nibble(pcm[frame * 2U], predictor[0], index_[0]);
        const uint8_t right = encode_nibble(pcm[frame * 2U + 1U], predictor[1], index_[1]);
        payload[frame - 1U] = static_cast<uint8_t>(left | (right << 4U));
    }
    return encoded_size;
}

std::size_t adpcm::encode_mono(const int16_t *pcm,
                               std::size_t samples,
                               uint8_t *output,
                               std::size_t output_capacity)
{
    const std::size_t encoded_size = sle_audio::adpcm_mono_encoded_size(samples);
    if (pcm == nullptr || output == nullptr || encoded_size == 0 || encoded_size > output_capacity) {
        return 0;
    }

    output[0] = sle_audio::adpcm_mono_packet_marker;
    put_u16_le(output + 1, static_cast<uint16_t>(samples));
    int32_t predictor = pcm[0];
    put_u16_le(output + 3, static_cast<uint16_t>(pcm[0]));
    output[5] = static_cast<uint8_t>(index_[0]);
    output[6] = 0;

    uint8_t *payload = output + sle_audio::adpcm_mono_packet_header_size;
    for (std::size_t sample = 1; sample < samples; ++sample) {
        const uint8_t nibble = encode_nibble(pcm[sample], predictor, index_[0]);
        const std::size_t byte_index = (sample - 1U) / 2U;
        if (((sample - 1U) & 1U) == 0) {
            payload[byte_index] = nibble;
        } else {
            payload[byte_index] = static_cast<uint8_t>(payload[byte_index] | (nibble << 4U));
        }
    }
    return encoded_size;
}

std::size_t adpcm::decode(const uint8_t *packet,
                          std::size_t packet_length,
                          int16_t *output,
                          std::size_t output_capacity) const
{
    if (packet == nullptr || output == nullptr || packet_length < sle_audio::adpcm_packet_header_size ||
        packet[0] != sle_audio::adpcm_packet_marker) {
        return 0;
    }

    const std::size_t interleaved_samples = get_u16_le(packet + 1);
    const std::size_t expected_size = sle_audio::adpcm_encoded_size(interleaved_samples);
    if (expected_size == 0 || expected_size != packet_length || interleaved_samples > output_capacity) {
        return 0;
    }

    int32_t predictor[2] = {
        static_cast<int16_t>(get_u16_le(packet + 3)),
        static_cast<int16_t>(get_u16_le(packet + 7)),
    };
    int index[2] = {packet[5], packet[9]};
    if (index[0] > 88 || index[1] > 88) {
        return 0;
    }

    output[0] = static_cast<int16_t>(predictor[0]);
    output[1] = static_cast<int16_t>(predictor[1]);
    const std::size_t frames = interleaved_samples / 2U;
    const uint8_t *payload = packet + sle_audio::adpcm_packet_header_size;
    for (std::size_t frame = 1; frame < frames; ++frame) {
        const uint8_t packed = payload[frame - 1U];
        output[frame * 2U] = decode_nibble(packed, predictor[0], index[0]);
        output[frame * 2U + 1U] = decode_nibble(packed >> 4U, predictor[1], index[1]);
    }
    return interleaved_samples;
}

std::size_t adpcm::decode_mono(const uint8_t *packet,
                               std::size_t packet_length,
                               int16_t *output,
                               std::size_t output_capacity) const
{
    if (packet == nullptr || output == nullptr || packet_length < sle_audio::adpcm_mono_packet_header_size ||
        packet[0] != sle_audio::adpcm_mono_packet_marker) {
        return 0;
    }

    const std::size_t samples = get_u16_le(packet + 1);
    const std::size_t expected_size = sle_audio::adpcm_mono_encoded_size(samples);
    if (expected_size == 0 || expected_size != packet_length || samples > output_capacity) {
        return 0;
    }

    int32_t predictor = static_cast<int16_t>(get_u16_le(packet + 3));
    int index = packet[5];
    if (index > 88) {
        return 0;
    }

    output[0] = static_cast<int16_t>(predictor);
    const uint8_t *payload = packet + sle_audio::adpcm_mono_packet_header_size;
    for (std::size_t sample = 1; sample < samples; ++sample) {
        const uint8_t packed = payload[(sample - 1U) / 2U];
        const uint8_t nibble = (((sample - 1U) & 1U) == 0) ? (packed & 0x0FU) : (packed >> 4U);
        output[sample] = decode_nibble(nibble, predictor, index);
    }
    return samples;
}
