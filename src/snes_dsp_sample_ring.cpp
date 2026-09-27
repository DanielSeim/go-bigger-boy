#include "gameboy/snes_dsp_sample_ring.hpp"

#include "gameboy/snes_dsp_gaussian.hpp"

#include <algorithm>
#include <cstdint>

namespace gameboy {
namespace {

std::int32_t floor_div(const std::int32_t value,
                       const std::int32_t divisor) noexcept {
    return value >= 0 ? value / divisor
                      : -(((-value) + divisor - 1) / divisor);
}

} // namespace

void SnesDspSampleRing::reset() noexcept {
    samples_.fill(0);
    phase_ = 0;
    write_position_ = 0;
}

void SnesDspSampleRing::key_on() noexcept {
    phase_ = 0;
    write_position_ = 0;
}

void SnesDspSampleRing::load_group(const Group& group) noexcept {
    for (unsigned index = 0; index < group.size(); ++index) {
        samples_[write_position_ + index] = static_cast<std::int16_t>(
            std::clamp<int>(group[index], -16384, 16383));
    }
    write_position_ = static_cast<std::uint8_t>(
        (static_cast<unsigned>(write_position_) + group.size()) % samples_.size());
}

SnesDspSampleRing::Group SnesDspSampleRing::window() const noexcept {
    Group result{};
    const auto base = static_cast<unsigned>(write_position_) + (phase_ >> 12);
    for (unsigned index = 0; index < result.size(); ++index) {
        result[index] = samples_[(base + index) % samples_.size()];
    }
    return result;
}

std::int16_t SnesDspSampleRing::interpolated() const noexcept {
    return SnesDspGaussian::interpolate(
        window(), static_cast<std::uint8_t>((phase_ >> 4) & 0xFFU));
}

void SnesDspSampleRing::advance_pitch(const std::uint16_t pitch,
                                      const std::int16_t previous_output,
                                      const bool modulate) noexcept {
    const auto base = static_cast<std::int32_t>(pitch & 0x3FFFU);
    auto effective = base;
    if (modulate) {
        const auto previous = std::clamp<int>(previous_output, -16384, 16383);
        effective += floor_div(floor_div(previous, 32) * base, 1024);
    }
    const auto next = static_cast<std::int32_t>(phase_ & 0x3FFFU) + effective;
    phase_ = static_cast<std::uint16_t>(std::clamp(next, 0, 0x7FFF));
}

} // namespace gameboy
