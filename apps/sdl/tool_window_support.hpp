#pragma once

#include <SDL3/SDL.h>

namespace gbb::sdl {

void draw_tool_button_background(SDL_Renderer* renderer, SDL_Window* window,
                                 const SDL_FRect& rect);
[[nodiscard]] bool confirm_discard_changes(SDL_Window* window,
                                            const char* message);

} // namespace gbb::sdl
