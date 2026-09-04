#include "frame_pacer.hpp"

#include <algorithm>
#include <thread>

namespace gbb::sdl {

FramePacer::FramePacer(const std::uint64_t cycles_per_frame,
                       const double clock_hz)
    : frame_duration_(static_cast<double>(cycles_per_frame) / clock_hz),
      next_frame_(Clock::now()) {}

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
        std::this_thread::sleep_until(next_frame_);
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
        std::this_thread::sleep_until(slice_end);
    }
}

} // namespace gbb::sdl
