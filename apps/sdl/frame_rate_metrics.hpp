#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace gbb::sdl {

struct FrameRateSample final {
    float fps{};
    float window_ms{};
    std::uint32_t frames{};
};

// Presentation-rate accounting kept independent from SDL so it can be tested
// with deterministic timestamps and reused by headless performance tests.
class FrameRateMetrics final {
public:
    using Clock = std::chrono::steady_clock;

    explicit FrameRateMetrics(
        const std::chrono::milliseconds window = std::chrono::milliseconds{500})
        : window_seconds_(std::chrono::duration<float>(window).count()) {}

    void reset() noexcept {
        window_start_ = {};
        window_frames_ = 0;
        fps_ = 0.0F;
    }

    [[nodiscard]] std::optional<FrameRateSample> observe(
        const Clock::time_point now) noexcept {
        if (window_start_.time_since_epoch().count() == 0) {
            window_start_ = now;
        }
        ++window_frames_;
        const auto elapsed = std::chrono::duration<float>(now - window_start_)
                                 .count();
        if (elapsed < window_seconds_ || elapsed <= 0.0F) return {};

        fps_ = static_cast<float>(window_frames_) / elapsed;
        const FrameRateSample sample{fps_, elapsed * 1000.0F,
                                     window_frames_};
        window_start_ = now;
        window_frames_ = 0;
        return sample;
    }

    [[nodiscard]] float fps() const noexcept { return fps_; }

private:
    Clock::time_point window_start_{};
    std::uint32_t window_frames_{};
    float window_seconds_{};
    float fps_{};
};

} // namespace gbb::sdl
