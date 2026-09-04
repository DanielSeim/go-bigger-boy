#include "tool_window_support.hpp"

namespace gbb::sdl {
namespace {

bool tool_button_hovered(SDL_Window* window, const SDL_FRect& rect) {
    if (window == nullptr) return false;
    float x = 0.0F;
    float y = 0.0F;
    if (!SDL_GetMouseState(&x, &y)) return false;
    return x >= rect.x && x <= rect.x + rect.w && y >= rect.y &&
           y <= rect.y + rect.h;
}

} // namespace

void draw_tool_button_background(SDL_Renderer* renderer, SDL_Window* window,
                                 const SDL_FRect& rect) {
    const auto hovered = tool_button_hovered(window, rect);
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer, hovered ? 40 : 28, hovered ? 74 : 47,
        hovered ? 98 : 68, 255));
    static_cast<void>(SDL_RenderFillRect(renderer, &rect));
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer, hovered ? 120 : 69, hovered ? 232 : 207,
        hovered ? 250 : 238, 255));
    static_cast<void>(SDL_RenderRect(renderer, &rect));
}

bool confirm_discard_changes(SDL_Window* window, const char* message) {
    constexpr SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Keep editing"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Discard"},
    };
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_WARNING, window, "Unsaved changes", message, 2,
        buttons, nullptr,
    };
    int selection = 0;
    return SDL_ShowMessageBox(&box, &selection) && selection == 1;
}

} // namespace gbb::sdl
