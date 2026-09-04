#include "window_event.hpp"

#include <SDL3/SDL.h>

#include <cassert>

int main() {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = 42;
    assert(gbb::sdl::event_window_id(event) == 42);

    event = {};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = 7;
    assert(gbb::sdl::event_window_id(event) == 7);

    event = {};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = 19;
    assert(gbb::sdl::event_window_id(event) == 19);

    event = {};
    event.type = SDL_EVENT_QUIT;
    assert(gbb::sdl::event_window_id(event) == 0);
    return 0;
}
