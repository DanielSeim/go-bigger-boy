#pragma once

#include "event_dispatch.hpp"

namespace gbb::sdl {

// Applies application-level policy around the SDL event pump. Individual
// event handlers remain in event_dispatch.cpp; this boundary coordinates
// ordering, shutdown prompts, dashboard state, and platform-specific hooks.
void process_events(SdlEventContext& context);

} // namespace gbb::sdl
