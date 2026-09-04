#pragma once

#include "gbb/core_registry.hpp"

namespace gbb {

// The Game Boy adapter owns its factory definition; other cores can expose an
// equivalent contribution without changing the generic registry.
[[nodiscard]] CoreFactory gameboy_core_factory();

} // namespace gbb
