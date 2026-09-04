#pragma once

#include "gbb/core_registry.hpp"

#include <vector>

namespace gbb {

// Built-in cores contribute factories through this seam instead of being
// selected inside CoreRegistry itself. A new statically linked core can add a
// factory contribution without changing probing or selection logic.
[[nodiscard]] std::vector<CoreFactory> built_in_core_factories();

} // namespace gbb
