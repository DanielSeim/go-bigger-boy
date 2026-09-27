#include "gameboy/snes_brr.hpp"

#include <algorithm>
#include <cstdint>

namespace gameboy {
namespace {

// Defined floor division for signed values, including negative inputs. This
// matches the arithmetic right shifts in the hardware filter equations
// without relying on implementation-defined signed C++ right shift.
std::int32_t floor_div_pow2(const std::int32_t value,
                            const unsigned shift) noexcept {
    const auto divisor = std::int32_t{1} << shift;
    return value >= 0 ? value / divisor
                      : -(((-value) + divisor - 1) / divisor);
}

std::int16_t signed_fifteen_bits(const std::int32_t value) noexcept {
    const auto clamped = std::clamp(value, -32768, 32767);
    const auto bits = static_cast<std::uint16_t>(clamped) & 0x7FFFU;
    const auto signed_bits = static_cast<std::int32_t>(bits);
    return static_cast<std::int16_t>(
        bits >= 0x4000U ? signed_bits - 0x8000 : signed_bits);
}

} // namespace

void SnesBrrDecoder::seed_history(const std::int16_t previous,
                                  const std::int16_t before_previous) noexcept {
    previous_ = static_cast<std::int16_t>(
        std::clamp<int>(previous, -16384, 16383));
    before_previous_ = static_cast<std::int16_t>(
        std::clamp<int>(before_previous, -16384, 16383));
}

SnesBrrDecoder::DecodedBlock SnesBrrDecoder::decode(
    const EncodedBlock& block) noexcept {
    DecodedBlock decoded;
    decoded.end = (block[0] & 1U) != 0;
    decoded.loop = (block[0] & 2U) != 0;
    for (unsigned group = 0; group < 4; ++group) {
        const auto samples = decode_group(
            block[0], {block[1 + group * 2], block[2 + group * 2]});
        for (unsigned index = 0; index < samples.size(); ++index) {
            decoded.samples[group * 4 + index] = samples[index];
        }
    }
    return decoded;
}

SnesBrrDecoder::DecodedGroup SnesBrrDecoder::decode_group(
    const std::uint8_t header, const EncodedGroup& bytes) noexcept {
    DecodedGroup decoded{};
    const auto shift = static_cast<unsigned>(header >> 4);
    const auto filter = static_cast<unsigned>((header >> 2) & 3U);
    for (unsigned index = 0; index < decoded.size(); ++index) {
        const auto packed = bytes[index / 2];
        const auto nibble = static_cast<unsigned>(
            index % 2 == 0 ? packed >> 4 : packed & 0x0FU);
        const auto signed_nibble = static_cast<std::int32_t>(nibble) -
                                   (nibble >= 8 ? 16 : 0);
        const auto residual = shift <= 12
            ? floor_div_pow2(signed_nibble * (std::int32_t{1} << shift), 1)
            : (signed_nibble < 0 ? -2048 : 0);
        const auto p1 = static_cast<std::int32_t>(previous_);
        const auto p2 = static_cast<std::int32_t>(before_previous_);
        auto sample = residual;
        switch (filter) {
        case 0: break;
        case 1:
            sample += p1 + floor_div_pow2(-p1, 4);
            break;
        case 2:
            sample += 2 * p1 + floor_div_pow2(-3 * p1, 5)
                    - p2 + floor_div_pow2(p2, 4);
            break;
        case 3:
            sample += 2 * p1 + floor_div_pow2(-13 * p1, 6)
                    - p2 + floor_div_pow2(3 * p2, 4);
            break;
        }
        const auto output = signed_fifteen_bits(sample);
        decoded[index] = output;
        before_previous_ = previous_;
        previous_ = output;
    }
    return decoded;
}

} // namespace gameboy
