#pragma once

#include <cstdint>

namespace gbb {

// A small core-neutral scheduler primitive. Callbacks advance one emulated
// instruction and may be backed by any core or transport participant.
using StepCallback = unsigned (*)(void* context);

struct BalancedAdvanceResult {
    unsigned first_cycles{};
    unsigned second_cycles{};
};

// Core-neutral progress watchdog used by link sessions. A participant can be
// busy without making progress for a bounded number of emulated cycles; the
// watchdog records that condition without knowing anything about serial
// hardware or a particular core.
class LinkWatchdog final {
public:
    explicit LinkWatchdog(std::uint64_t timeout_cycles) noexcept
        : timeout_cycles_(timeout_cycles) {}

    void reset(std::uint64_t progress = 0) noexcept;
    void observe(unsigned elapsed_cycles, bool active,
                 std::uint64_t progress) noexcept;
    void mark_timeout() noexcept { timed_out_ = true; }

    [[nodiscard]] bool timed_out() const noexcept { return timed_out_; }
    [[nodiscard]] std::uint64_t stalled_cycles() const noexcept {
        return stalled_cycles_;
    }

private:
    std::uint64_t timeout_cycles_{};
    std::uint64_t stalled_cycles_{};
    std::uint64_t progress_marker_{};
    bool timed_out_{};
};

// Advance both participants toward target_cycles without allowing one side to
// run an entire frontend slice ahead of the other. Instruction boundaries are
// indivisible, so either total may exceed the target by its final instruction.
[[nodiscard]] BalancedAdvanceResult advance_balanced(
    void* first_context, StepCallback first_step, void* second_context,
    StepCallback second_step, unsigned target_cycles);

} // namespace gbb
