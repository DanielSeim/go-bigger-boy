#pragma once

#include <array>
#include <cstdint>

namespace gameboy {

// Four-point S-DSP Gaussian interpolation for decoded signed 15-bit BRR
// samples, oldest to newest. This does not advance pitch or manage the
// physical 12-sample BRR ring; the caller supplies the selected four samples.
class SnesDspGaussian final {
public:
    [[nodiscard]] static std::int16_t interpolate(
        const std::array<std::int16_t, 4>& samples,
        std::uint8_t fraction) noexcept;
};

} // namespace gameboy
