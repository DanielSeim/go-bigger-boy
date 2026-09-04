#include "window_event.hpp"

namespace gbb::sdl {

SDL_WindowID event_window_id(const SDL_Event& event) noexcept {
    if (event.type >= SDL_EVENT_WINDOW_FIRST &&
        event.type <= SDL_EVENT_WINDOW_LAST) {
        return event.window.windowID;
    }
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return event.key.windowID;
    case SDL_EVENT_TEXT_INPUT:
        return event.text.windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return event.button.windowID;
    case SDL_EVENT_MOUSE_MOTION:
        return event.motion.windowID;
    case SDL_EVENT_MOUSE_WHEEL:
        return event.wheel.windowID;
    default:
        return 0;
    }
}

} // namespace gbb::sdl
