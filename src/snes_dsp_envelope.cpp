#include "gameboy/snes_dsp_envelope.hpp"

#include "gameboy/snes_dsp_voice_math.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace gameboy {
namespace {

constexpr std::array<unsigned, 32> rates{
    0, 2048, 1536, 1280, 1024, 768, 640, 512,
    384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12,
    10, 8, 6, 5, 4, 3, 2, 1,
};

constexpr std::array<unsigned, 32> offsets{
    0, 0, 1040, 536, 0, 1040, 536, 0,
    1040, 536, 0, 1040, 536, 0, 1040, 536,
    0, 1040, 536, 0, 1040, 536, 0, 1040,
    536, 0, 1040, 536, 0, 1040, 0, 0,
};

constexpr std::uint16_t max_envelope = 0x7FF;

std::int32_t exponential_decrease(const std::uint16_t value) noexcept {
    // ((E-1)>>8)+1 with arithmetic right shift; for E=0 the result is 0.
    return value == 0 ? 0 : static_cast<std::int32_t>((value + 255U) >> 8);
}

} // namespace

void SnesDspRateClock::advance() noexcept {
    counter_ = counter_ == 0 ? 0x77FFU
                             : static_cast<std::uint16_t>(counter_ - 1);
}

bool SnesDspRateClock::event(const std::uint8_t rate) const noexcept {
    const auto index = static_cast<unsigned>(rate & 31U);
    return index != 0 && (static_cast<unsigned>(counter_) + offsets[index]) %
                             rates[index] == 0;
}

void SnesDspEnvelope::reset() noexcept {
    envelope_ = 0;
    preclamp_ = 0;
    phase_ = Phase::release;
}

void SnesDspEnvelope::key_on() noexcept {
    envelope_ = 0;
    preclamp_ = 0;
    phase_ = Phase::attack;
}

void SnesDspEnvelope::clock(const SnesDspRateClock& rate_clock,
                            const std::uint8_t adsr1,
                            const std::uint8_t adsr2,
                            const std::uint8_t gain) noexcept {
    if (phase_ == Phase::release) {
        envelope_ = SnesDspVoiceMath::release_step(envelope_);
        preclamp_ = envelope_;
        return;
    }

    auto next = static_cast<std::int32_t>(envelope_);
    if ((adsr1 & 0x80U) != 0) {
        switch (phase_) {
        case Phase::attack: {
            const auto attack = static_cast<unsigned>(adsr1 & 15U);
            const auto rate = static_cast<std::uint8_t>(
                attack == 15 ? 31 : attack * 2 + 1);
            if (rate_clock.event(rate)) next += attack == 15 ? 1024 : 32;
            break;
        }
        case Phase::decay: {
            const auto decay = static_cast<std::uint8_t>(
                ((adsr1 >> 4) & 7U) * 2U + 16U);
            if (rate_clock.event(decay)) next -= exponential_decrease(envelope_);
            break;
        }
        case Phase::sustain:
            if (rate_clock.event(adsr2 & 31U)) {
                next -= exponential_decrease(envelope_);
            }
            break;
        case Phase::release: break;
        }
    } else if ((gain & 0x80U) == 0) {
        next = SnesDspVoiceMath::direct_gain(gain);
    } else if (rate_clock.event(gain & 31U)) {
        switch ((gain >> 5) & 3U) {
        case 0: next -= 32; break;
        case 1: next -= exponential_decrease(envelope_); break;
        case 2: next += 32; break;
        case 3: next += preclamp_ < 0x600 ? 32 : 8; break;
        }
    }

    preclamp_ = next;
    envelope_ = static_cast<std::uint16_t>(
        std::clamp(next, std::int32_t{0}, std::int32_t{max_envelope}));
    // The ADSR phase comparators remain active even when GAIN mode supplies
    // the envelope adjustment, matching the S-DSP's independent state latch.
    if (phase_ == Phase::attack && (next < 0 || next > max_envelope)) {
        phase_ = Phase::decay;
    } else if (phase_ == Phase::decay &&
               (envelope_ >> 8) == (adsr2 >> 5)) {
        phase_ = Phase::sustain;
    }
}

} // namespace gameboy
