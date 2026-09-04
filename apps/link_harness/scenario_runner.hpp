#pragma once

#include "scenario_state.hpp"

#include <cstdint>
#include <functional>

namespace gbb::link_harness {

struct ScenarioRunResult {
    std::uint64_t frames_run{};
};

using ScenarioFrameCallback =
    std::function<void(std::uint64_t frame, AutoInputState& input_state)>;
using ScenarioStopCallback = std::function<bool()>;

// Runs one scenario's frame policy. Transport-specific code is supplied as a
// callback; this shared layer guarantees identical frame counting and early
// termination semantics for local and TCP runs.
[[nodiscard]] ScenarioRunResult run_scenario(
    std::uint64_t frame_limit, Scenario scenario,
    AutoInputState& input_state, const ScenarioFrameCallback& frame_callback,
    const ScenarioStopCallback& stop_callback);

} // namespace gbb::link_harness
