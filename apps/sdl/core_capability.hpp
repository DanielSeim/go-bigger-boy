#pragma once

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

} // namespace gbb::sdl
