#include "event_dispatch.hpp"

#include <SDL3/SDL.h>

#include <cassert>

int main() {
    int called = 0;
    gbb::sdl::pump_events(
        [] { return false; },
        [&called](const SDL_Event&) { ++called; });
    assert(called == 0);

    assert(SDL_Init(SDL_INIT_EVENTS));
    SDL_Event pushed{};
    pushed.type = SDL_EVENT_USER;
    assert(SDL_PushEvent(&pushed) == 1);
    int user_events = 0;
    gbb::sdl::pump_events(
        [&user_events] { return user_events == 0; },
        [&user_events](const SDL_Event& event) {
            if (event.type == SDL_EVENT_USER) ++user_events;
        });
    assert(user_events == 1);
#ifndef __ANDROID__
    assert(gbb::sdl::is_voxel_video_mode(gameboy::VideoMode::voxel_diorama));
    assert(gbb::sdl::is_voxel_video_mode(gameboy::VideoMode::voxel_shape));
    assert(gbb::sdl::is_voxel_video_mode(gameboy::VideoMode::voxel_popup));
    assert(!gbb::sdl::is_voxel_video_mode(gameboy::VideoMode::nearest));
#endif
    SDL_Quit();
    return 0;
}
