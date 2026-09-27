#pragma once

#include <array>
#include <cstdint>

namespace gameboy {

// Isolated S-DSP sample-position and 12-sample BRR ring state. The caller
// supplies decoded four-sample groups and handles key-on delay, BRR reads,
    // applicability of pitch modulation, and register polling outside this class.
class SnesDspSampleRing final {
public:
    using Group = std::array<std::int16_t, 4>;

    void reset() noexcept;
    // Key-on resets position, but leaves the physical sample ring intact.
    void key_on() noexcept;
    void load_group(const Group& group) noexcept;

    // For each output sample: read window()/interpolated() first; if
    // group_due(), load one group; then advance_pitch(). The caller handles
    // the three key-on prefill groups before the first audible sample.
    [[nodiscard]] bool group_due() const noexcept { return phase_ >= 0x4000U; }
    [[nodiscard]] Group window() const noexcept;
    [[nodiscard]] std::int16_t interpolated() const noexcept;
    // The caller enables modulation only for eligible non-noise voices 1-7.
    // previous_output is the preceding voice's pre-volume output sample.
    void advance_pitch(std::uint16_t pitch, std::int16_t previous_output = 0,
                       bool modulate = false) noexcept;

    [[nodiscard]] std::uint16_t phase() const noexcept { return phase_; }
    [[nodiscard]] std::uint8_t write_position() const noexcept { return write_position_; }

private:
    std::array<std::int16_t, 12> samples_{};
    std::uint16_t phase_{};
    std::uint8_t write_position_{};
};

} // namespace gameboy
