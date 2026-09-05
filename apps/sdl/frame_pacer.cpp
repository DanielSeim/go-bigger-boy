#include "frame_pacer.hpp"

#include <algorithm>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace gbb::sdl {

namespace {

#ifdef _WIN32
// Windows may use a coarse system timer for standard C++ sleeps. A 16.7 ms
// frame deadline can then be missed by an entire timer quantum, producing a
// visible 30 FPS cadence even when emulation itself is fast. Keep the higher
// resolution scoped to the lifetime of the frontend rather than changing the
// process-wide timer policy during static initialization.
class WindowsTimerResolution {
  public:
    WindowsTimerResolution() noexcept
        : active_(timeBeginPeriod(1) == TIMERR_NOERROR) {}

    ~WindowsTimerResolution() {
        if (active_) static_cast<void>(timeEndPeriod(1));
    }

    WindowsTimerResolution(const WindowsTimerResolution&) = delete;
    WindowsTimerResolution& operator=(const WindowsTimerResolution&) = delete;

  private:
    bool active_{};
};

void enable_windows_timer_resolution() noexcept {
    static const WindowsTimerResolution resolution;
    static_cast<void>(resolution);
}
#endif

void wait_until_precise(const FramePacer::Clock::time_point deadline) {
    // Leave a short tail for yielding instead of trusting sleep_until to hit
    // a sub-millisecond deadline. This avoids both coarse-timer overshoot and
    // the sustained busy-spin that would result from spinning for a whole
    // frame.
    constexpr auto yield_tail = std::chrono::milliseconds(2);
    while (true) {
        const auto now = FramePacer::Clock::now();
        if (deadline <= now) return;
        const auto remaining = deadline - now;
        if (remaining > yield_tail) {
            std::this_thread::sleep_for(remaining - yield_tail);
        } else {
            std::this_thread::yield();
        }
    }
}

} // namespace

FramePacer::FramePacer(const std::uint64_t cycles_per_frame,
                       const double clock_hz)
    : frame_duration_(static_cast<double>(cycles_per_frame) / clock_hz),
      next_frame_(Clock::now()) {
#ifdef _WIN32
    enable_windows_timer_resolution();
#endif
}

void FramePacer::set_timing(const std::uint64_t cycles_per_frame,
                            const double clock_hz) {
    if (cycles_per_frame == 0 || !(clock_hz > 0.0)) return;
    frame_duration_ =
        std::chrono::duration<double>(static_cast<double>(cycles_per_frame) /
                                      clock_hz);
    reset();
}

void FramePacer::reset() {
    next_frame_ = Clock::now();
}

void FramePacer::advance() {
    next_frame_ +=
        std::chrono::duration_cast<Clock::duration>(frame_duration_);
}

void FramePacer::wait(const std::function<void()>& poll) {
    const auto now = Clock::now();
    if (next_frame_ <= now) {
        if (now - next_frame_ > std::chrono::milliseconds(100)) {
            next_frame_ = now;
        }
        return;
    }

    if (!poll) {
        wait_until_precise(next_frame_);
        return;
    }

    // Poll in short slices while pacing so a remote serial bit does not wait
    // for a complete video frame. The callback does not affect emulation time.
    while (true) {
        const auto current = Clock::now();
        if (next_frame_ <= current) break;
        poll();
        const auto slice_end = std::min(
            next_frame_, current + std::chrono::milliseconds(1));
        wait_until_precise(slice_end);
    }
}

} // namespace gbb::sdl
