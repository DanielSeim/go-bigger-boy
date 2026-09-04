#include "scenario_runner.hpp"

namespace gbb::link_harness {

ScenarioRunResult run_scenario(
    const std::uint64_t frame_limit, const Scenario scenario,
    AutoInputState& input_state, const ScenarioFrameCallback& frame_callback,
    const ScenarioStopCallback& stop_callback) {
    ScenarioRunResult result;
    for (std::uint64_t frame = 0; frame < frame_limit; ++frame) {
        frame_callback(frame, input_state);
        result.frames_run = frame + 1;
        if (scenario != Scenario::none && stop_callback()) break;
    }
    return result;
}

} // namespace gbb::link_harness
