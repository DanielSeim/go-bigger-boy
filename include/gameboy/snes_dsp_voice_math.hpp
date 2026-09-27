#pragma once

#include <cstdint>

namespace gameboy {

// Fixed-point S-DSP voice stages after interpolation/noise selection.
// Callers provide signed 15-bit samples; this does not schedule envelopes,
// interpolate BRR data, or produce a complete DSP output frame.
class SnesDspVoiceMath final {
public:
    [[nodiscard]] static std::uint16_t direct_gain(std::uint8_t gain) noexcept;
    [[nodiscard]] static std::uint16_t release_step(std::uint16_t envelope) noexcept;
    [[nodiscard]] static std::int16_t apply_envelope(std::int16_t sample15,
                                                      std::uint16_t envelope) noexcept;
    [[nodiscard]] static std::int16_t expand_to_16bit(std::int16_t sample15) noexcept;
    [[nodiscard]] static std::int16_t apply_channel_volume(std::int16_t sample16,
                                                            std::uint8_t volume) noexcept;
    [[nodiscard]] static std::int16_t saturating_add(std::int16_t lhs,
                                                     std::int16_t rhs) noexcept;
};

} // namespace gameboy
