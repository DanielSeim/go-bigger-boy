#include "event_dispatch.hpp"

#include "gameboy/display_palette.hpp"
#include "input_mapping.hpp"
#include "input_lifecycle.hpp"
#ifdef __ANDROID__
#include "android_touch_input.hpp"
#endif
#include "settings_persistence.hpp"

namespace gbb::sdl {

void handle_window_lifecycle_event(const SDL_Event& event,
                                   SdlEventContext& context) {
#ifdef __ANDROID__
    if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
        clear_touch_buttons(context.core.get(), context.sdl);
        if (context.emulator != nullptr) release_all_buttons(*context.emulator);
        // Android's library and settings activities temporarily cover the SDL
        // surface. These menus are transient UI state and must not survive
        // that hand-off, otherwise the link panel can reappear when the game
        // resumes even though the user opened the general menu or library.
        context.sdl.android_menu_visible = false;
        context.sdl.android_link_menu_visible = false;
        flush_battery_safely(context.core.get());
        context.paused = true;
        return;
    }
    if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        context.sdl.android_menu_visible = false;
        context.sdl.android_link_menu_visible = false;
        clear_touch_buttons(context.core.get(), context.sdl);
        if (context.emulator != nullptr) release_all_buttons(*context.emulator);
        context.display_palette =
            load_display_palette(context.preference_path);
        refresh_touch_settings(context.sdl, context.preference_path);
        if (context.emulator != nullptr) {
            context.emulator->set_dmg_compatibility_colors(
                gameboy::display_palettes[context.display_palette]
                    .cgb_compatibility);
        }
        context.paused = false;
        return;
    }
#endif
    if (event.type != SDL_EVENT_WINDOW_FOCUS_LOST) return;
    if (context.emulator) {
#ifndef __ANDROID__
        context.input_movie.release_all(*context.emulator);
        release_all_buttons(*context.emulator);
#else
        release_all_buttons(*context.emulator);
#endif
    }
    if (context.link_emulator != nullptr) {
        release_all_buttons(*context.link_emulator);
    }
    context.fast_forward = false;
    context.rewind = false;
    context.sdl.voxel_camera_dragging = false;
    static_cast<void>(SDL_CaptureMouse(false));
#ifdef __ANDROID__
    clear_touch_buttons(context.core.get(), context.sdl);
#endif
    stop_rumble(context.sdl);
}

} // namespace gbb::sdl
