#pragma once

#include <SDL3/SDL.h>

namespace gbb::sdl {

// Render desktop-tool text at a readable size while retaining SDL's built-in
// debug font as a fallback for builds without SDL_ttf.
void render_tool_text(SDL_Renderer* renderer, float x, float y,
                      const char* value);
void clear_tool_text_cache(SDL_Renderer* renderer) noexcept;

void draw_tool_button_background(SDL_Renderer* renderer, SDL_Window* window,
                                 const SDL_FRect& rect);
void draw_tool_focus_outline(SDL_Renderer* renderer, const SDL_FRect& rect);
[[nodiscard]] int cycle_tool_focus(int current, int count, bool reverse) noexcept;
[[nodiscard]] SDL_FRect tool_close_button_rect(int width) noexcept;
[[nodiscard]] bool tool_close_button_hit(int width, int height, float x,
                                         float y) noexcept;
void draw_tool_close_button(SDL_Renderer* renderer, SDL_Window* window,
                            int width);
[[nodiscard]] bool confirm_discard_changes(SDL_Window* window,
                                            const char* message);

} // namespace gbb::sdl
