#include "emulation_policy.hpp"

#include <iostream>

namespace {

gbb::sdl::EmulationPolicyInput active_input() {
    gbb::sdl::EmulationPolicyInput input;
    input.core_loaded = true;
    return input;
}

bool expect(const gbb::sdl::EmulationPolicyInput& input,
            const gbb::sdl::EmulationMode mode,
            const unsigned batch,
            const bool rewind = false) {
    const auto plan = gbb::sdl::plan_emulation(input);
    return plan.mode == mode && plan.frame_batch_factor == batch &&
           plan.restores_rewind_state() == rewind;
}

} // namespace

int main() {
    using gbb::sdl::EmulationMode;

    if (!expect({}, EmulationMode::idle, 1)) {
        std::cerr << "missing-core gate regression\n";
        return 1;
    }

    auto input = active_input();
    if (!expect(input, EmulationMode::ordinary, 1)) {
        std::cerr << "ordinary execution regression\n";
        return 1;
    }
    input.paused = true;
    if (!expect(input, EmulationMode::idle, 1)) {
        std::cerr << "pause gate regression\n";
        return 1;
    }
    input = active_input();
    input.debugger_stepped = true;
    if (!expect(input, EmulationMode::idle, 1)) {
        std::cerr << "debugger gate regression\n";
        return 1;
    }
    input = active_input();
    input.debugger_paused = true;
    if (!expect(input, EmulationMode::idle, 1)) {
        std::cerr << "debugger pause gate regression\n";
        return 1;
    }
    input = active_input();
    input.dashboard_visible = true;
    if (!expect(input, EmulationMode::idle, 1)) {
        std::cerr << "dashboard gate regression\n";
        return 1;
    }
    input = active_input();
    input.dialog_active = true;
    if (!expect(input, EmulationMode::idle, 1)) {
        std::cerr << "dialog gate regression\n";
        return 1;
    }
    input = active_input();
    input.configuring = true;
    if (!expect(input, EmulationMode::idle, 1)) {
        std::cerr << "binding configuration gate regression\n";
        return 1;
    }
    input = active_input();
    input.cheat_visible = true;
    if (!expect(input, EmulationMode::idle, 1)) {
        std::cerr << "cheat visibility gate regression\n";
        return 1;
    }
    input = active_input();
    input.cheat_fetching = true;
    if (!expect(input, EmulationMode::idle, 1)) {
        std::cerr << "cheat fetch gate regression\n";
        return 1;
    }

    input = active_input();
    input.rewind_requested = true;
    if (!expect(input, EmulationMode::rewind, 1, true)) {
        std::cerr << "rewind selection regression\n";
        return 1;
    }
    input.local_link_active = true;
    if (!expect(input, EmulationMode::local_link, 1)) {
        std::cerr << "rewind/local-link precedence regression\n";
        return 1;
    }
    input.remote_transport_connected = true;
    if (!expect(input, EmulationMode::local_link, 1)) {
        std::cerr << "local-link/remote precedence regression\n";
        return 1;
    }

    input = active_input();
    input.fast_forward = true;
    if (!expect(input, EmulationMode::ordinary,
                gbb::sdl::fast_forward_frame_batch_factor)) {
        std::cerr << "fast-forward batch regression\n";
        return 1;
    }
    input.remote_transport_connected = true;
    if (!expect(input, EmulationMode::remote,
                gbb::sdl::fast_forward_frame_batch_factor)) {
        std::cerr << "remote mode precedence regression\n";
        return 1;
    }
    input = active_input();
    input.input_replaying = true;
    if (!expect(input, EmulationMode::remote, 1)) {
        std::cerr << "replay mode regression\n";
        return 1;
    }
    return 0;
}
