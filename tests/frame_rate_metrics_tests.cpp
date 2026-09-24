#include "frame_rate_metrics.hpp"

#include <chrono>
#include <cmath>
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
    using Metrics = gbb::sdl::FrameRateMetrics;
    using namespace std::chrono_literals;

    Metrics metrics{500ms};
    const auto start = Metrics::Clock::time_point{1s};
    check(!metrics.observe(start).has_value(),
          "the first frame starts a measurement window");
    check(!metrics.observe(start + 250ms).has_value(),
          "an incomplete measurement window does not publish a sample");
    const auto sample = metrics.observe(start + 500ms);
    check(sample.has_value(), "a completed measurement window publishes a sample");
    if (sample.has_value()) {
        check(sample->frames == 3,
              "a sample counts every frame observed in its window");
        check(std::fabs(sample->fps - 6.0F) < 0.01F,
              "FPS is derived from frames divided by elapsed time");
        check(std::fabs(sample->window_ms - 500.0F) < 0.01F,
              "the sample reports the elapsed window in milliseconds");
    }

    metrics.reset();
    check(metrics.fps() == 0.0F, "reset clears the last reported FPS");
    check(!metrics.observe(start + 1s).has_value(),
          "reset starts a fresh measurement window");
    return failures == 0 ? 0 : 1;
}
