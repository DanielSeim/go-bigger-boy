#include "gbb/core_runtime.hpp"

#include "gameboy/emulator.hpp"

namespace gbb {
namespace {

template <typename Core, typename Step>
FrameAdvanceResult advance_impl(Core& core, const unsigned cycle_budget,
                                Step&& step) {
    FrameAdvanceResult result{0, core.frame_ready()};
    while (!result.frame_ready && result.cycles < cycle_budget) {
        const auto stepped = step(core);
        // A core must normally report positive instruction costs, but avoid
        // spinning forever if a third-party core violates that contract.
        if (stepped == 0) break;
        result.cycles += stepped;
        result.frame_ready = core.frame_ready();
    }
    return result;
}

} // namespace

FrameAdvanceResult advance_to_frame(EmulatorCore& core,
                                    const unsigned cycle_budget) {
    return advance_impl(core, cycle_budget,
                        [](EmulatorCore& value) {
                            return value.step_instruction();
                        });
}

FrameAdvanceResult advance_to_frame(gameboy::Emulator& emulator,
                                    const unsigned cycle_budget) {
    return advance_impl(emulator, cycle_budget,
                        [](gameboy::Emulator& value) {
                            return value.step();
                        });
}

} // namespace gbb
