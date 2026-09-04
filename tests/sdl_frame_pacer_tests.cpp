#include "frame_pacer.hpp"

#include <chrono>
#include <iostream>

namespace {
int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    using gbb::sdl::FramePacer;
    FramePacer pacer;
    const auto initial = pacer.deadline();
    check(pacer.frame_duration().count() > 0.016 &&
              pacer.frame_duration().count() < 0.017,
          "frame pacer uses the Game Boy frame cadence");
    pacer.advance();
    check(pacer.deadline() > initial,
          "advancing the pacer schedules the next deadline");
    pacer.reset();
    check(pacer.deadline() >= initial,
          "reset moves pacing to the current clock");
    pacer.set_timing(1000, 1000.0);
    check(pacer.frame_duration().count() > 0.99 &&
              pacer.frame_duration().count() < 1.01,
          "pacer can adopt a core-provided clock and frame cadence");
    pacer.wait();
    return failures == 0 ? 0 : 1;
}
