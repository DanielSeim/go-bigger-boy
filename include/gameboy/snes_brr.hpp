#pragma once

#include <array>
#include <cstdint>

namespace gameboy {

// Decode one 9-byte SNES BRR block into sixteen signed 15-bit samples.
// This is only the BRR data stage: it does not implement DSP interpolation,
// envelopes, voice timing, echo, or SGB audio playback.
class SnesBrrDecoder final {
public:
    using EncodedBlock = std::array<std::uint8_t, 9>;

    struct DecodedBlock {
        std::array<std::int16_t, 16> samples{};
        bool end{};
        bool loop{};
    };

    [[nodiscard]] DecodedBlock decode(const EncodedBlock& block) noexcept;
    // Power-on/test reset of prediction history. A voice key-on does not
    // necessarily clear the physical DSP ring buffer's previous samples.
    void reset() noexcept { previous_ = 0; before_previous_ = 0; }

private:
    std::int16_t previous_{};
    std::int16_t before_previous_{};
};

} // namespace gameboy
