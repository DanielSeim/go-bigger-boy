#include "event_policy.hpp"

#include "event_dispatch.hpp"

#include "gbb/log.hpp"

#ifdef __ANDROID__
#include "android_bridge.hpp"
#endif

namespace gbb::sdl {

void process_events(SdlEventContext& context) {
    auto& core = context.core;
    auto& emulator = context.emulator;
    auto* const link_emulator = context.link_emulator;
    auto& sdl = context.sdl;
    auto& configuring = context.configuring;
    auto& pending_rom = context.pending_rom;
    auto& dashboard_visible = context.dashboard_visible;
    auto& running = context.running;
#ifndef __ANDROID__
    auto& paused = context.paused;
    auto& fullscreen = context.fullscreen;
    const auto remote_link_active = context.remote_link_active;
    auto& input_movie = context.input_movie;
#ifdef _WIN32
    auto& desktop_menu = context.desktop_menu;
#endif
#endif
#ifdef __ANDROID__
    // The Java activity intercepts Android's back callback and sets this flag.
    // Handle it here, on SDL's thread, rather than allowing the activity to
    // finish while the emulator is still writing its save file.
    if (const auto back_context = take_android_back_request()) {
        auto callback_context = gbb::LogContextScope::exact(*back_context);
        gbb::log_frontend_info("Android back request accepted");
        if (dashboard_visible && core != nullptr) {
            dashboard_visible = false;
        } else if (context.leave_game) {
            context.leave_game();
        }
    }
#endif
#ifdef _WIN32
    desktop_menu.update(core != nullptr,
                        core ? core->descriptor().capabilities
                             : gbb::CoreCapability::none,
                        paused, fullscreen,
                        input_movie.recording(), display_palette, sdl.video_mode,
                        link_emulator != nullptr, remote_link_active);
    handle_desktop_menu_event(context);
#endif
    bool close_prompt_shown = false;
    const auto request_close = [&]() {
        if (close_prompt_shown || !running) return;
        close_prompt_shown = true;
        if (context.confirm_exit && context.confirm_exit()) running = false;
    };
    pump_events(
        [&running] { return running; },
        [&](const SDL_Event& event) {
#ifndef __ANDROID__
        if (handle_desktop_tool_event(event, context)) return;
#endif
        // On desktop, voxel mode uses the mouse to control pitch and
        // center-axis yaw. Android touch input has its own gesture path below;
        // SDL can synthesize mouse events for touches, so handling those here
        // would bypass the voxel-orbit preference entirely.
#ifndef __ANDROID__
        handle_desktop_voxel_mouse_event(event, context);
#endif
        switch (event.type) {
        case SDL_EVENT_QUIT:
            request_close();
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (event.window.windowID == SDL_GetWindowID(sdl.window)) {
                request_close();
            }
            break;
        case SDL_EVENT_DROP_FILE:
            if (event.drop.data != nullptr) pending_rom = event.drop.data;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
#ifdef __ANDROID__
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_AC_BACK) {
                if (dashboard_visible && core != nullptr) {
                    dashboard_visible = false;
                } else if (context.leave_game) {
                    context.leave_game();
                }
                break;
            }
#endif
            if (dashboard_visible) {
                static_cast<void>(handle_dashboard_key_event(event, context));
                break;
            }
            if (configuring) {
                static_cast<void>(handle_keyboard_binding_event(event, context));
                break;
            }

            handle_gameplay_key_event(event, context);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_MOUSE_MOTION:
            handle_mouse_event(event, context);
            break;
#ifdef __ANDROID__
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            handle_touch_event(event, context);
            break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            handle_window_lifecycle_event(event, context);
            break;
#endif
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            handle_window_lifecycle_event(event, context);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            handle_gamepad_device_event(event, context);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            handle_gamepad_event(event, context);
            break;
        default:
            break;
        }
        });
}

} // namespace gbb::sdl
