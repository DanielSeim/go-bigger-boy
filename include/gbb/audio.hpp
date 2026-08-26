#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gbb {

// Reduces interleaved PCM by averaging adjacent frames. This is intended for
// accelerated playback where selecting every Nth frame would alias pulse and
// noise channels badly.
[[nodiscard]] std::vector<std::int16_t> downsample_audio_box(
    const std::vector<std::int16_t>& samples, std::size_t channels,
    std::size_t factor);

[[nodiscard]] constexpr std::size_t audio_queue_bytes(
    const unsigned sample_rate, const unsigned channels,
    const unsigned milliseconds) noexcept {
    return static_cast<std::size_t>(sample_rate) * channels *
           sizeof(std::int16_t) * milliseconds / 1000;
}

} // namespace gbb
