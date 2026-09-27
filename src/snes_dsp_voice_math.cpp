#include "gameboy/snes_dsp_voice_math.hpp"

#include <algorithm>
#include <cstdint>

namespace gameboy {
namespace {

std::int32_t floor_div_pow2(const std::int32_t value,
                            const unsigned shift) noexcept {
    const auto divisor = std::int32_t{1} << shift;
    return value >= 0 ? value / divisor
                      : -(((-value) + divisor - 1) / divisor);
}

} // namespace

std::uint16_t SnesDspVoiceMath::direct_gain(const std::uint8_t gain) noexcept {
    // Direct mode is only meaningful with ADSR1.7=0 and GAIN.7=0. The caller
    // selects that mode; mask the high bit so this helper cannot exceed 11 bits.
    return static_cast<std::uint16_t>((gain & 0x7FU) << 4);
}

std::uint16_t SnesDspVoiceMath::release_step(
    const std::uint16_t envelope) noexcept {
    const auto bounded = std::min<unsigned>(envelope, 0x7FFU);
    return static_cast<std::uint16_t>(bounded > 8 ? bounded - 8 : 0);
}

std::int16_t SnesDspVoiceMath::apply_envelope(
    const std::int16_t sample15,
    const std::uint16_t envelope) noexcept {
    const auto bounded_sample = std::clamp<int>(sample15, -16384, 16383);
    const auto bounded_envelope = std::min<unsigned>(envelope, 0x7FFU);
    return static_cast<std::int16_t>(floor_div_pow2(
        bounded_sample * static_cast<std::int32_t>(bounded_envelope), 11));
}

std::int16_t SnesDspVoiceMath::expand_to_16bit(
    const std::int16_t sample15) noexcept {
    return static_cast<std::int16_t>(
        2 * std::clamp<int>(sample15, -16384, 16383));
}

std::int16_t SnesDspVoiceMath::apply_channel_volume(
    const std::int16_t sample16,
    const std::uint8_t volume) noexcept {
    const auto signed_volume = static_cast<std::int32_t>(volume) -
                               (volume >= 0x80U ? 256 : 0);
    return static_cast<std::int16_t>(std::clamp(
        floor_div_pow2(static_cast<std::int32_t>(sample16) * signed_volume, 7),
        -32768, 32767));
}

std::int16_t SnesDspVoiceMath::saturating_add(
    const std::int16_t lhs,
    const std::int16_t rhs) noexcept {
    return static_cast<std::int16_t>(std::clamp(
        static_cast<std::int32_t>(lhs) + rhs, -32768, 32767));
}

} // namespace gameboy
