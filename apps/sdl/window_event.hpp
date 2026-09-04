#pragma once

#include <SDL3/SDL.h>

namespace gbb::sdl {

// Returns the owning window for SDL events that can be routed to a tool or
// the main frontend. Unknown event types return zero.
[[nodiscard]] SDL_WindowID event_window_id(const SDL_Event& event) noexcept;

} // namespace gbb::sdl
