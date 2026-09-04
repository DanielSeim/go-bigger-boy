#include "gbb/link_scheduler.hpp"

#include <stdexcept>

namespace gbb {

void LinkWatchdog::reset(const std::uint64_t progress) noexcept {
    stalled_cycles_ = 0;
    progress_marker_ = progress;
    timed_out_ = false;
}

void LinkWatchdog::observe(const unsigned elapsed_cycles, const bool active,
                           const std::uint64_t progress) noexcept {
    if (timed_out_) return;
    if (!active) {
        stalled_cycles_ = 0;
    } else if (progress == progress_marker_) {
        const auto elapsed = static_cast<std::uint64_t>(elapsed_cycles);
        stalled_cycles_ = elapsed > UINT64_MAX - stalled_cycles_
                               ? UINT64_MAX
                               : stalled_cycles_ + elapsed;
    } else {
        stalled_cycles_ = 0;
    }
    progress_marker_ = progress;
    if (active && stalled_cycles_ >= timeout_cycles_) timed_out_ = true;
}

BalancedAdvanceResult advance_balanced(
    void* first_context, const StepCallback first_step, void* second_context,
    const StepCallback second_step, const unsigned target_cycles) {
    if (target_cycles == 0) return {};
    if (first_context == nullptr || second_context == nullptr ||
        first_step == nullptr || second_step == nullptr) {
        throw std::invalid_argument("balanced link scheduler requires two callbacks");
    }

    BalancedAdvanceResult result{};
    while (result.first_cycles < target_cycles ||
           result.second_cycles < target_cycles) {
        if (result.first_cycles <= result.second_cycles &&
            result.first_cycles < target_cycles) {
            result.first_cycles += first_step(first_context);
        } else if (result.second_cycles < target_cycles) {
            result.second_cycles += second_step(second_context);
        } else if (result.first_cycles < target_cycles) {
            result.first_cycles += first_step(first_context);
        } else {
            break;
        }
    }
    return result;
}

} // namespace gbb
