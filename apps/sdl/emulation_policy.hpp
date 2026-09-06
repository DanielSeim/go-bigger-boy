#pragma once

#include <cstdint>

namespace gbb::sdl {

// The SDL loop has several mutually-exclusive execution paths. Keep the
// selection policy independent from the concrete emulator and from SDL so it
// can be reused by another frontend and tested without opening a window.
enum class EmulationMode {
    idle,
    rewind,
    local_link,
    ordinary,
    remote,
};

// Keep the acceleration factor at the execution-policy boundary so every
// frontend reports and applies the same fast-forward contract.
inline constexpr std::uint8_t fast_forward_frame_batch_factor = 4;

struct EmulationPolicyInput final {
    bool core_loaded{};
    bool debugger_stepped{};
    bool paused{};
    bool debugger_paused{};
    bool dashboard_visible{};
    bool configuring{};
    bool cheat_visible{};
    bool cheat_fetching{};
    bool dialog_active{};
    bool rewind_requested{};
    bool local_link_active{};
    bool remote_transport_connected{};
    bool input_replaying{};
    bool fast_forward{};
};

struct EmulationPlan final {
    EmulationMode mode{EmulationMode::idle};
    std::uint8_t frame_batch_factor{1};

    [[nodiscard]] constexpr bool should_run() const noexcept {
        return mode != EmulationMode::idle;
    }

    [[nodiscard]] constexpr bool restores_rewind_state() const noexcept {
        return mode == EmulationMode::rewind;
    }
};

[[nodiscard]] constexpr EmulationPlan plan_emulation(
    const EmulationPolicyInput& input) noexcept {
    if (!input.core_loaded || input.debugger_stepped || input.paused ||
        input.debugger_paused || input.dashboard_visible || input.configuring ||
        input.cheat_visible || input.cheat_fetching || input.dialog_active) {
        return {};
    }

    // A linked local session owns two timelines, so rewind snapshots are not
    // valid there. This mirrors the runtime's existing precedence: a rewind
    // request wins only for a standalone core.
    if (input.rewind_requested && !input.local_link_active) {
        return {EmulationMode::rewind, 1};
    }

    const auto batch = input.fast_forward
                           ? fast_forward_frame_batch_factor
                           : static_cast<std::uint8_t>(1);
    if (input.local_link_active) {
        return {EmulationMode::local_link, batch};
    }
    if (!input.remote_transport_connected && !input.input_replaying) {
        return {EmulationMode::ordinary, batch};
    }
    return {EmulationMode::remote, batch};
}

} // namespace gbb::sdl
