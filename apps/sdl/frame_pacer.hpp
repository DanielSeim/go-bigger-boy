#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

namespace gbb::sdl {

// Keeps presentation pacing independent from the SDL event loop. A poll
// callback can be supplied while sleeping so network transports remain
// responsive without changing emulated CPU timing.
class FramePacer {
  public:
    using Clock = std::chrono::steady_clock;

    explicit FramePacer(std::uint64_t cycles_per_frame = 70'224,
                        double clock_hz = 4'194'304.0);

    // Update pacing when a core with a different clock or frame cadence is
    // loaded. The default remains the Game Boy timing used before a ROM is
    // selected.
    void set_timing(std::uint64_t cycles_per_frame, double clock_hz);
    void reset();
    void advance();
    void wait(const std::function<void()>& poll = {});

    [[nodiscard]] Clock::time_point deadline() const noexcept {
        return next_frame_;
    }
    [[nodiscard]] std::chrono::duration<double> frame_duration() const noexcept {
        return frame_duration_;
    }

  private:
    std::chrono::duration<double> frame_duration_;
    Clock::time_point next_frame_;
};

} // namespace gbb::sdl
