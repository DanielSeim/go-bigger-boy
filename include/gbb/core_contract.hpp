#pragma once

#include "gbb/core.hpp"

#include <string>

namespace gbb {

// Validate the invariants every frontend is allowed to rely on, without
// knowing which concrete emulator implements the core.  Keeping this check at
// the boundary turns malformed adapters into an immediate, actionable error
// instead of a later texture/audio/input failure.
[[nodiscard]] bool validate_core_contract(const EmulatorCore& core,
                                          std::string& error);

} // namespace gbb
