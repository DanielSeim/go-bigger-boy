#pragma once

#include "gameboy/emulator.hpp"
#include "gbb/core.hpp"

namespace gbb::sdl {

// Frontend tools may still use a concrete adapter, but the adapter is not
// enough to make a feature valid.  Keep the capability check next to the
// pointer check so a future core cannot accidentally expose a Game Boy-only
// tool merely because it happens to provide an adapter.
[[nodiscard]] constexpr bool supports(const CoreCapability capabilities,
                                      const CoreCapability capability) noexcept {
    return has_capability(capabilities, capability);
}

[[nodiscard]] inline bool supports(const EmulatorCore* core,
                                   const CoreCapability capability) noexcept {
    return core != nullptr && supports(core->descriptor().capabilities,
                                       capability);
}

// Optional SDL services are deliberately a view, not an ownership layer. The
// generic core remains the source of truth for capabilities; the concrete
// adapter is only returned when both the capability and pointer are present.
// Keeping this policy in one place prevents a future core from routing events
// to an incompatible or stale Game Boy tool pointer.
struct CoreServices final {
    EmulatorCore* core{};
    gameboy::Emulator* emulator{};

    [[nodiscard]] gameboy::Emulator* get(
        const CoreCapability capability) const noexcept {
        return supports(core, capability) ? emulator : nullptr;
    }

    [[nodiscard]] gameboy::Emulator* debugger() const noexcept {
        return get(CoreCapability::debugger);
    }
    [[nodiscard]] gameboy::Emulator* sprite_editor() const noexcept {
        return get(CoreCapability::sprite_editor);
    }
    [[nodiscard]] gameboy::Emulator* cheats() const noexcept {
        return get(CoreCapability::cheats);
    }
    [[nodiscard]] gameboy::Emulator* link_cable() const noexcept {
        return get(CoreCapability::link_cable);
    }
};

// Compatibility name for the existing SDL call sites. New code should use
// CoreServices so optional feature access reads as a service boundary.
using GameBoyToolAdapter = CoreServices;

} // namespace gbb::sdl
