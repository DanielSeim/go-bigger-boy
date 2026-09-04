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

// A concrete Game Boy adapter is non-owning and only valid for tools whose
// capability is advertised by the owning generic core. Keeping both checks in
// this view prevents a future core from routing events to an incompatible or
// stale adapter pointer.
struct GameBoyToolAdapter final {
    EmulatorCore* core{};
    gameboy::Emulator* emulator{};

    [[nodiscard]] gameboy::Emulator* get(
        const CoreCapability capability) const noexcept {
        return supports(core, capability) ? emulator : nullptr;
    }
};

} // namespace gbb::sdl
