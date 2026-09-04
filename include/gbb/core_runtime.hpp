#pragma once

#include "gbb/core.hpp"

namespace gameboy {
class Emulator;
}

namespace gbb {

// Result of a bounded execution slice. The cycle count is the sum of the
// instructions actually executed; the last instruction may cross the budget
// because instructions are indivisible at the core boundary.
struct FrameAdvanceResult {
    unsigned cycles{};
    bool frame_ready{};
};

// Execute instructions until a video frame is ready or cycle_budget is
// reached. This function never consumes the frame, leaving presentation
// ownership with the caller. A zero budget is a valid no-op.
[[nodiscard]] FrameAdvanceResult advance_to_frame(EmulatorCore& core,
                                                   unsigned cycle_budget);

// Transitional overload for frontends that still retain a concrete Game Boy
// handle for optional debugger/link features. It follows exactly the generic
// contract above and can be removed once those frontends finish their adapter
// migration.
[[nodiscard]] FrameAdvanceResult advance_to_frame(gameboy::Emulator& emulator,
                                                   unsigned cycle_budget);

} // namespace gbb
