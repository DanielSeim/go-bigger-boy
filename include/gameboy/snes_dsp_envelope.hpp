#pragma once

#include <cstdint>

namespace gameboy {

// One global S-DSP envelope/noise rate counter. Call advance() once per
// 32 kHz output sample, then query event() for each voice's selected rate.
// This models the documented sample-phase counter, not the 32-cycle
// interleaving of register reads across eight voices.
class SnesDspRateClock final {
public:
    void reset() noexcept { counter_ = 0; }
    void advance() noexcept;
    [[nodiscard]] bool event(std::uint8_t rate) const noexcept;
    [[nodiscard]] std::uint16_t counter() const noexcept { return counter_; }

private:
    std::uint16_t counter_{};
};

class SnesDspEnvelope final {
public:
    enum class Phase : std::uint8_t { attack, decay, sustain, release };

    void reset() noexcept;
    void key_on() noexcept;
    void key_off() noexcept { phase_ = Phase::release; }
    void clock(const SnesDspRateClock& rates, std::uint8_t adsr1,
               std::uint8_t adsr2, std::uint8_t gain) noexcept;

    [[nodiscard]] std::uint16_t value() const noexcept { return envelope_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }

private:
    std::uint16_t envelope_{};
    std::int32_t preclamp_{};
    Phase phase_{Phase::release};
};

} // namespace gameboy
