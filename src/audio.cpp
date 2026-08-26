#include "gbb/audio.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gbb {

std::vector<std::int16_t> downsample_audio_box(
    const std::vector<std::int16_t>& samples, const std::size_t channels,
    const std::size_t factor) {
    if (channels == 0 || factor == 0) {
        throw std::invalid_argument("Audio channel count and factor must be positive");
    }
    if (factor == 1 || samples.empty()) return samples;

    const auto frames = samples.size() / channels;
    std::vector<std::int16_t> output;
    output.reserve(((frames + factor - 1) / factor) * channels);
    for (std::size_t first = 0; first < frames; first += factor) {
        const auto count = std::min(factor, frames - first);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            std::int64_t sum = 0;
            for (std::size_t frame = 0; frame < count; ++frame) {
                sum += samples[(first + frame) * channels + channel];
            }
            const auto average = sum / static_cast<std::int64_t>(count);
            output.push_back(static_cast<std::int16_t>(std::clamp<std::int64_t>(
                average, std::numeric_limits<std::int16_t>::min(),
                std::numeric_limits<std::int16_t>::max())));
        }
    }
    return output;
}

} // namespace gbb
