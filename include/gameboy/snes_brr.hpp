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
    using EncodedGroup = std::array<std::uint8_t, 2>;
    using DecodedGroup = std::array<std::int16_t, 4>;

    struct DecodedBlock {
        std::array<std::int16_t, 16> samples{};
        bool end{};
        bool loop{};
    };

    [[nodiscard]] DecodedBlock decode(const EncodedBlock& block) noexcept;
    // Decode two BRR data bytes using the current header. Prediction history
    // continues across groups and blocks.
    [[nodiscard]] DecodedGroup decode_group(std::uint8_t header,
                                            const EncodedGroup& bytes) noexcept;
    // Power-on/test reset of sequential prediction history. Real key-on
    // prediction can read the physical DSP ring instead of these last two
    // sequential outputs; that scheduling detail is not modeled here.
    void reset() noexcept { previous_ = 0; before_previous_ = 0; }

private:
    std::int16_t previous_{};
    std::int16_t before_previous_{};
};

} // namespace gameboy
