#include "event_dispatch.hpp"
#include "emulation_session.hpp"
#include "gbb/dashboard_navigation.hpp"
#include "gameboy/display_palette.hpp"
#include "input_mapping.hpp"
#include "settings_model.hpp"
#include "settings_persistence.hpp"
#include "core_capability.hpp"

#include <algorithm>
#include <exception>

#ifndef _WIN32
#ifdef __ANDROID__
#include "android_touch_input.hpp"
#include "gbb/touch_control.hpp"
#endif
#endif


namespace gbb::sdl {

constexpr float voxel_camera_yaw_drag_limit = 45.0F;

void pump_events(const EventPumpContinue& should_continue,
                 const EventHandler& handle_event) {
    SDL_Event event;
    while (should_continue() && SDL_PollEvent(&event)) {
        handle_event(event);
    }
}

void handle_gamepad_device_event(const SDL_Event& event,
                                 SdlEventContext& context) {
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        if (context.sdl.gamepad == nullptr) {
            context.sdl.gamepad = SDL_OpenGamepad(event.gdevice.which);
            context.sdl.rumble_warning_shown = false;
        }
    } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED &&
               context.sdl.gamepad != nullptr &&
               SDL_GetGamepadID(context.sdl.gamepad) == event.gdevice.which) {
        SDL_CloseGamepad(context.sdl.gamepad);
        context.sdl.gamepad = nullptr;
        context.sdl.rumble_output_active = false;
        context.sdl.rumble_warning_shown = false;
    }
}

#ifndef GBB_EVENT_DISPATCH_CORE_ONLY
void handle_gamepad_event(const SDL_Event& event, SdlEventContext& context) {
    if (context.dashboard_visible) {
        if (event.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN) return;
        const auto item_count = context.dashboard_item_count
                                    ? context.dashboard_item_count()
                                    : std::size_t{0};
        const auto button =
            static_cast<SDL_GamepadButton>(event.gbutton.button);
        if (button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
            context.dashboard_selection = gbb::desktop::dashboard_move_selection(
                context.dashboard_selection, item_count, -1);
        } else if (button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
            context.dashboard_selection = gbb::desktop::dashboard_move_selection(
                context.dashboard_selection, item_count, 1);
        } else if (button == SDL_GAMEPAD_BUTTON_SOUTH) {
            if (context.activate_dashboard) {
                context.activate_dashboard(context.dashboard_selection);
            }
        } else if (button == SDL_GAMEPAD_BUTTON_EAST && context.core) {
            context.dashboard_visible = false;
        }
        return;
    }

    if (context.configuring &&
        context.configuring->device == BindingDevice::gamepad) {
        if (event.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN) return;
        const auto pressed =
            static_cast<SDL_GamepadButton>(event.gbutton.button);
        const auto duplicate = std::find(
            context.bindings.gamepad_buttons.begin(),
            context.bindings.gamepad_buttons.end(), pressed);
        if (duplicate != context.bindings.gamepad_buttons.end() &&
            static_cast<std::size_t>(
                duplicate - context.bindings.gamepad_buttons.begin()) !=
                context.configuring->index) {
            if (context.report_error) {
                context.report_error("That gamepad button is already assigned.");
            }
            return;
        }
        context.bindings.gamepad_buttons[context.configuring->index] = pressed;
        ++context.configuring->index;
        if (context.configuring->index == context.bindings.gamepad_buttons.size()) {
            context.configuring.reset();
            save_bindings(context.preference_path, context.bindings);
            static_cast<void>(SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_INFORMATION, "Gamepad controls",
                "Gamepad bindings saved.", context.sdl.window));
        }
        if (context.update_title) context.update_title();
        return;
    }

    if (context.core) {
        if (const auto button = gamepad_button(
                context.bindings, event.gbutton.button)) {
#ifndef __ANDROID__
            if (context.emulator &&
                supports(context.core.get(), CoreCapability::debugger)) {
                context.input_movie.set_button(
                    *context.emulator, *button,
                    event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            } else {
                context.core->set_input(
                    core_input_id(*button),
                    event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            }
#else
            context.core->set_input(
                core_input_id(*button),
                event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
#endif
        }
    }
}
#endif

#ifndef __ANDROID__
bool is_voxel_video_mode(const gameboy::VideoMode mode) noexcept {
    return mode == gameboy::VideoMode::voxel_diorama ||
           mode == gameboy::VideoMode::voxel_shape ||
           mode == gameboy::VideoMode::voxel_popup;
}

#ifndef GBB_EVENT_DISPATCH_CORE_ONLY
bool handle_desktop_tool_event(const SDL_Event& event,
                               SdlEventContext& context) {
    if (context.emulator == nullptr || context.core == nullptr) return false;
    if (supports(context.core.get(), CoreCapability::cheats) &&
        context.cheat_manager.handle_event(event)) {
        return true;
    }
    if (supports(context.core.get(), CoreCapability::sprite_editor) &&
        context.sprite_editor.handle_event(event, context.emulator)) {
        return true;
    }
    if (supports(context.core.get(), CoreCapability::debugger) &&
        context.tas_editor.handle_event(event)) {
        return true;
    }
    if (supports(context.core.get(), CoreCapability::debugger) &&
        context.debugger.handle_event(event, context.emulator)) {
        return true;
    }
    return false;
}

void handle_desktop_voxel_mouse_event(const SDL_Event& event,
                                      SdlEventContext& context) {
    if (context.emulator == nullptr || context.core == nullptr ||
        !supports(context.core.get(), CoreCapability::scene_layers) ||
        context.dashboard_visible ||
        !is_voxel_video_mode(context.sdl.video_mode)) {
        return;
    }
    auto& sdl = context.sdl;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT) {
        sdl.voxel_camera_dragging = true;
        static_cast<void>(SDL_CaptureMouse(true));
    } else if (event.type == SDL_EVENT_MOUSE_MOTION &&
               sdl.voxel_camera_dragging &&
               (event.motion.state & SDL_BUTTON_LMASK) != 0) {
        sdl.voxel_camera_pitch_offset = std::clamp(
            sdl.voxel_camera_pitch_offset - event.motion.yrel * 0.25F,
            -75.0F, 75.0F);
        sdl.voxel_camera_yaw_offset = std::clamp(
            sdl.voxel_camera_yaw_offset + event.motion.xrel * 0.25F,
            -voxel_camera_yaw_drag_limit, voxel_camera_yaw_drag_limit);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
               event.button.button == SDL_BUTTON_LEFT) {
        sdl.voxel_camera_dragging = false;
        static_cast<void>(SDL_CaptureMouse(false));
    }
}
#endif
#endif

#if defined(_WIN32) && !defined(GBB_EVENT_DISPATCH_CORE_ONLY)
void handle_desktop_menu_event(SdlEventContext& context) {
    auto& sdl = context.sdl;
    auto* const emulator = context.emulator;
    auto& core = context.core;
    auto* const link_emulator = context.link_emulator;
    const auto& preference_path = context.preference_path;
    auto& bindings = context.bindings;
    auto& configuring = context.configuring;
    const auto input_movie_active =
        context.input_movie.mode() != InputMovie::Mode::idle;
    const auto menu_command = context.desktop_menu.take_command();
    const auto menu_value = static_cast<int>(menu_command);
    const auto palette_first =
        static_cast<int>(DesktopMenuCommand::palette_first);
    const auto video_first = static_cast<int>(DesktopMenuCommand::video_first);
    if (menu_command == DesktopMenuCommand::none) return;
    const auto has_capability = [&](const CoreCapability capability) {
        return supports(core.get(), capability) && emulator != nullptr;
    };

    if (menu_value >= palette_first &&
        menu_value < palette_first +
                         static_cast<int>(gameboy::display_palettes.size())) {
        context.display_palette =
            static_cast<std::size_t>(menu_value - palette_first);
        save_display_palette(preference_path, context.display_palette);
        const auto colors =
            gameboy::display_palettes[context.display_palette].cgb_compatibility;
        if (core && supports(core.get(),
                             CoreCapability::compatibility_palette)) {
            core->set_compatibility_colors(colors);
        }
        if (link_emulator) link_emulator->set_dmg_compatibility_colors(colors);
        return;
    }
    if (menu_value >= video_first &&
        menu_value < video_first +
                         static_cast<int>(gameboy::video_modes.size())) {
        sdl.video_mode = gameboy::video_modes[
            static_cast<std::size_t>(menu_value - video_first)].mode;
        save_video_mode(preference_path, sdl.video_mode);
        if (!configure_video_pipeline(sdl, sdl.video_mode) &&
            context.report_error) {
            context.report_error("Could not configure the selected video pipeline.");
        }
        return;
    }

    switch (menu_command) {
    case DesktopMenuCommand::open_rom:
        if (context.open_rom_dialog) context.open_rom_dialog();
        break;
    case DesktopMenuCommand::library:
        if (link_emulator != nullptr) context.link_toggle_requested = true;
        if (context.open_library) context.open_library();
        break;
    case DesktopMenuCommand::save_state:
        if (core) {
            try {
                save_quick_state(preference_path, *core);
                static_cast<void>(SDL_ShowSimpleMessageBox(
                    SDL_MESSAGEBOX_INFORMATION, "Save state", "State saved.",
                    sdl.window));
            } catch (const std::exception& error) {
                if (context.report_error) context.report_error(error.what());
            }
        }
        break;
    case DesktopMenuCommand::load_state:
        if (core) {
            try {
                load_quick_state(preference_path, *core);
                context.rewind_history.clear();
                release_all_buttons(*core);
                sdl.audio.clear();
            } catch (const std::exception& error) {
                if (context.report_error) context.report_error(error.what());
            }
        }
        break;
    case DesktopMenuCommand::exit_app:
        if (context.confirm_exit && context.confirm_exit()) context.running = false;
        break;
    case DesktopMenuCommand::pause:
        if (core) {
            context.paused = !context.paused;
            if (emulator) context.input_movie.release_all(*emulator);
            else release_all_buttons(*core);
            if (context.update_title) context.update_title();
        }
        break;
    case DesktopMenuCommand::reset:
        if (emulator && !input_movie_active) context.reset_requested = true;
        break;
    case DesktopMenuCommand::link_session:
        if (has_capability(CoreCapability::link_cable)) {
            context.link_toggle_requested = true;
        }
        break;
    case DesktopMenuCommand::link_retry:
        if (link_emulator != nullptr) context.link_retry_requested = true;
        break;
    case DesktopMenuCommand::remote_host:
        if (has_capability(CoreCapability::link_cable)) {
            context.remote_host_requested = true;
        }
        break;
    case DesktopMenuCommand::remote_join:
        if (has_capability(CoreCapability::link_cable)) {
            context.remote_join_requested = true;
        }
        break;
    case DesktopMenuCommand::remote_stop:
        if (context.remote_link_active) context.remote_stop_requested = true;
        break;
    case DesktopMenuCommand::fullscreen:
        context.fullscreen = !context.fullscreen;
        if (!SDL_SetWindowFullscreen(sdl.window, context.fullscreen)) {
            context.fullscreen = !context.fullscreen;
            if (context.report_error) context.report_error(SDL_GetError());
        }
        break;
    case DesktopMenuCommand::controls: {
        if (emulator) release_all_buttons(*emulator);
        const auto action = show_controls_dialog(sdl.window, bindings);
        if (action == ControlsAction::reset) {
            bindings = InputBindings{};
            save_bindings(preference_path, bindings);
        } else if (action == ControlsAction::keyboard ||
                   action == ControlsAction::gamepad) {
            if (action == ControlsAction::gamepad && sdl.gamepad == nullptr) {
                if (context.report_error) {
                    context.report_error("Connect a gamepad before configuring it.");
                }
            } else {
                begin_binding_configuration(
                    bindings, context.configuration_backup, configuring,
                    action == ControlsAction::keyboard
                        ? BindingDevice::keyboard
                        : BindingDevice::gamepad);
            }
        }
        if (context.update_title) context.update_title();
        break;
    }
    case DesktopMenuCommand::gameshark:
        if (has_capability(CoreCapability::cheats)) {
            release_all_buttons(*emulator);
            context.cheat_manager.open(sdl.window);
        }
        break;
    case DesktopMenuCommand::debugger:
        if (has_capability(CoreCapability::debugger)) {
            release_all_buttons(*emulator);
            context.debugger.toggle(sdl.window);
        }
        break;
    case DesktopMenuCommand::record_input:
        if (has_capability(CoreCapability::debugger)) {
            context.debugger.request_record_toggle();
        }
        break;
    case DesktopMenuCommand::replay_input:
        if (has_capability(CoreCapability::debugger)) {
            context.debugger.request_replay();
        }
        break;
    case DesktopMenuCommand::tas_editor:
        if (has_capability(CoreCapability::debugger)) {
            context.debugger.request_tas_editor();
        }
        break;
    case DesktopMenuCommand::sprite_editor:
        if (has_capability(CoreCapability::sprite_editor)) {
            context.debugger.request_sprite_editor();
        }
        break;
    case DesktopMenuCommand::shortcuts:
        if (emulator) release_all_buttons(*emulator);
        if (context.show_help) context.show_help();
        break;
    case DesktopMenuCommand::about:
        if (context.show_about) context.show_about();
        break;
    default:
        break;
    }
}
#endif

void handle_mouse_event(const SDL_Event& event, SdlEventContext& context) {
    auto& sdl = context.sdl;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event.button.button != SDL_BUTTON_LEFT) return;
        const auto window_x = event.button.x;
        const auto window_y = event.button.y;
        auto x = window_x;
        auto y = window_y;
        static_cast<void>(SDL_RenderCoordinatesFromWindow(
            sdl.renderer, x, y, &x, &y));
        if (context.dashboard_visible) {
            if (context.dashboard_row_at) {
                if (const auto selected = context.dashboard_row_at(x, y)) {
                    context.dashboard_selection = *selected;
                    if (context.activate_dashboard) {
                        context.activate_dashboard(*selected);
                    }
                }
            }
        }
#ifndef _WIN32
        else if (
#ifdef __ANDROID__
            android_menu_button_hit(sdl, window_x, window_y)
#else
            x < 20.0F && y < 17.0F
#endif
        ) {
            if (context.open_library) context.open_library();
        }
#endif
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        if (context.dashboard_visible && context.dashboard_item_count) {
            context.dashboard_selection = gbb::desktop::dashboard_scroll_selection(
                context.dashboard_selection, context.dashboard_item_count(),
                event.wheel.y > 0 ? -1 : event.wheel.y < 0 ? 1 : 0);
        }
    } else if (event.type == SDL_EVENT_MOUSE_MOTION &&
               context.dashboard_visible && context.dashboard_row_at) {
        auto x = event.motion.x;
        auto y = event.motion.y;
        static_cast<void>(SDL_RenderCoordinatesFromWindow(
            sdl.renderer, x, y, &x, &y));
        if (const auto selected = context.dashboard_row_at(x, y)) {
            context.dashboard_selection = *selected;
        }
    }
}

bool handle_dashboard_key_event(const SDL_Event& event,
                                SdlEventContext& context) {
    if (!context.dashboard_visible) return false;
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) return true;
    const auto item_count = context.dashboard_item_count
                                ? context.dashboard_item_count()
                                : std::size_t{0};
    if (event.key.key == SDLK_UP) {
        context.dashboard_selection = gbb::desktop::dashboard_move_selection(
            context.dashboard_selection, item_count, -1);
    } else if (event.key.key == SDLK_DOWN) {
        context.dashboard_selection = gbb::desktop::dashboard_move_selection(
            context.dashboard_selection, item_count, 1);
    } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_SPACE) {
        if (context.activate_dashboard) {
            context.activate_dashboard(context.dashboard_selection);
        }
    } else if (event.key.key == SDLK_F1) {
        if (context.show_help) context.show_help();
    } else if (event.key.key == SDLK_O) {
        if (context.open_rom_dialog) context.open_rom_dialog();
    } else if (event.key.key == SDLK_ESCAPE) {
        if (context.core) {
            context.dashboard_visible = false;
        } else if (context.confirm_exit && context.confirm_exit()) {
            context.running = false;
        }
    }
    return true;
}

#ifndef GBB_EVENT_DISPATCH_CORE_ONLY
bool handle_keyboard_binding_event(const SDL_Event& event,
                                   SdlEventContext& context) {
    if (!context.configuring) return false;
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) return true;
    auto& configuration = *context.configuring;
    if (event.key.key == SDLK_ESCAPE) {
        context.bindings = context.configuration_backup;
        context.configuring.reset();
        if (context.update_title) context.update_title();
        return true;
    }
    if (configuration.device != BindingDevice::keyboard) return true;
    if (configuration.slot == 1 && event.key.key == SDLK_SPACE) {
        context.bindings.keys[configuration.index][1] = SDLK_UNKNOWN;
        configuration.slot = 0;
        ++configuration.index;
        if (configuration.index == context.bindings.keys.size()) {
            context.configuring.reset();
            save_bindings(context.preference_path, context.bindings);
            static_cast<void>(SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_INFORMATION, "Keyboard controls",
                "Keyboard bindings saved.", context.sdl.window));
        }
        if (context.update_title) context.update_title();
        return true;
    }
    if (reserved_gameplay_key(context.bindings, event.key.key)) {
        if (context.report_error) {
            context.report_error(
                "That key is reserved for an emulator shortcut.");
        }
        return true;
    }
    bool duplicate = false;
    for (std::size_t index = 0; index < context.bindings.keys.size(); ++index) {
        for (std::size_t slot = 0;
             slot < context.bindings.keys[index].size(); ++slot) {
            if (context.bindings.keys[index][slot] == event.key.key &&
                (index != configuration.index || slot != configuration.slot)) {
                duplicate = true;
            }
        }
    }
    if (duplicate) {
        if (context.report_error) {
            context.report_error("That key is already assigned.");
        }
        return true;
    }
    context.bindings.keys[configuration.index][configuration.slot] =
        event.key.key;
    if (configuration.slot == 0) {
        configuration.slot = 1;
    } else {
        configuration.slot = 0;
        ++configuration.index;
        if (configuration.index == context.bindings.keys.size()) {
            context.configuring.reset();
            save_bindings(context.preference_path, context.bindings);
            static_cast<void>(SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_INFORMATION, "Keyboard controls",
                "Keyboard bindings saved.", context.sdl.window));
        }
    }
    if (context.update_title) context.update_title();
    return true;
}

void handle_gameplay_key_event(const SDL_Event& event,
                               SdlEventContext& context) {
    auto* const emulator = context.emulator;
    auto& core = context.core;
    auto* const link_emulator = context.link_emulator;
    auto& sdl = context.sdl;
    const auto& preference_path = context.preference_path;
    auto& bindings = context.bindings;
    auto& configuring = context.configuring;
    auto& pending_rom = context.pending_rom;
    auto& paused = context.paused;
    auto& fast_forward = context.fast_forward;
    auto& rewind = context.rewind;
    auto& rewind_history = context.rewind_history;
    auto& reset_requested = context.reset_requested;
    auto& link_toggle_requested = context.link_toggle_requested;
    auto& link_retry_requested = context.link_retry_requested;
    auto& remote_host_requested = context.remote_host_requested;
    auto& remote_join_requested = context.remote_join_requested;
    const auto remote_link_active = context.remote_link_active;
    const auto supports_link =
        supports(core.get(), CoreCapability::link_cable) && emulator != nullptr;
#ifndef __ANDROID__
    const auto replaying_input = context.input_movie.replaying();
    const auto input_movie_active =
        context.input_movie.mode() != InputMovie::Mode::idle;
    auto& input_movie = context.input_movie;
    auto& debugger = context.debugger;
    auto& cheat_manager = context.cheat_manager;
    auto& cheat_pause_restore = context.cheat_pause_restore;
    const auto update_download_active = context.update_download_active;
    auto& update_cancel_requested = context.update_cancel_requested;
#else
    constexpr auto replaying_input = false;
    constexpr auto input_movie_active = false;
#endif

    if (!replaying_input && core &&
        shortcut_pressed(bindings, shortcut_fast_forward, event.key.key)) {
        fast_forward = event.type == SDL_EVENT_KEY_DOWN;
    } else if (!input_movie_active && core &&
               shortcut_pressed(bindings, shortcut_rewind, event.key.key)) {
        rewind = link_emulator == nullptr && event.type == SDL_EVENT_KEY_DOWN;
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == bindings.shortcuts[shortcut_save_state] &&
               core && !replaying_input) {
        try {
            save_quick_state(preference_path, *core);
            static_cast<void>(SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_INFORMATION, "Save state", "State saved.",
                sdl.window));
        } catch (const std::exception& error) {
            if (context.report_error) context.report_error(error.what());
        }
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == bindings.shortcuts[shortcut_load_state] &&
               core && !input_movie_active) {
        try {
            load_quick_state(preference_path, *core);
            rewind_history.clear();
            release_all_buttons(*core);
            sdl.audio.clear();
            static_cast<void>(SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_INFORMATION, "Load state", "State loaded.",
                sdl.window));
        } catch (const std::exception& error) {
            if (context.report_error) context.report_error(error.what());
        }
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_O &&
               (event.key.mod & SDL_KMOD_CTRL) != 0) {
        if (context.open_rom_dialog) context.open_rom_dialog();
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_L &&
               (event.key.mod & SDL_KMOD_CTRL) != 0 &&
               (event.key.mod & SDL_KMOD_SHIFT) == 0) {
        if (emulator) release_all_buttons(*emulator);
        if (link_emulator != nullptr && supports_link) {
            link_toggle_requested = true;
        }
        if (context.open_library) context.open_library();
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_K &&
               (event.key.mod & SDL_KMOD_CTRL) != 0) {
        if (emulator) release_all_buttons(*emulator);
        const auto action = show_controls_dialog(sdl.window, bindings);
        if (action == ControlsAction::reset) {
            bindings = InputBindings{};
            save_bindings(preference_path, bindings);
            static_cast<void>(SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_INFORMATION, "Controls",
                "Keyboard and gamepad bindings restored to defaults.",
                sdl.window));
        } else if (action == ControlsAction::keyboard ||
                   action == ControlsAction::gamepad) {
            if (action == ControlsAction::gamepad && sdl.gamepad == nullptr) {
                if (context.report_error) {
                    context.report_error("Connect a gamepad before configuring it.");
                }
            } else {
                begin_binding_configuration(
                    bindings, context.configuration_backup, configuring,
                    action == ControlsAction::keyboard
                        ? BindingDevice::keyboard
                        : BindingDevice::gamepad);
            }
        }
        if (context.update_title) context.update_title();
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_G &&
               (event.key.mod & SDL_KMOD_CTRL) != 0 &&
               supports(core.get(), CoreCapability::cheats) && emulator) {
#ifndef __ANDROID__
        release_all_buttons(*emulator);
        if (!cheat_manager.visible()) {
            cheat_pause_restore = paused;
            paused = true;
            if (context.update_title) context.update_title();
        }
        cheat_manager.open(sdl.window);
#endif
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_P &&
               (event.key.mod & SDL_KMOD_CTRL) != 0) {
        if (context.choose_palette) context.choose_palette();
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key >= SDLK_1 && event.key.key <= SDLK_9 &&
               (event.key.mod & SDL_KMOD_CTRL) != 0) {
        const auto index = static_cast<std::size_t>(event.key.key - SDLK_1);
        if (index < context.recent.size()) pending_rom = context.recent[index];
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_R &&
               (event.key.mod & SDL_KMOD_CTRL) != 0 &&
               (event.key.mod & SDL_KMOD_SHIFT) == 0 && core &&
               !input_movie_active) {
        reset_requested = true;
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_L &&
               (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) ==
               (SDL_KMOD_CTRL | SDL_KMOD_SHIFT) && supports_link) {
        link_toggle_requested = true;
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_R &&
               (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) ==
                   (SDL_KMOD_CTRL | SDL_KMOD_SHIFT) &&
               (link_emulator != nullptr || remote_link_active)) {
        link_retry_requested = true;
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_H &&
               (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) ==
               (SDL_KMOD_CTRL | SDL_KMOD_SHIFT) && supports_link) {
        remote_host_requested = true;
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_J &&
               (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) ==
               (SDL_KMOD_CTRL | SDL_KMOD_SHIFT) && supports_link) {
        remote_join_requested = true;
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_SPACE && core) {
        paused = !paused;
#ifndef __ANDROID__
        if (emulator) input_movie.release_all(*emulator);
        else release_all_buttons(*core);
#else
        if (core) release_all_buttons(*core);
#endif
        if (context.update_title) context.update_title();
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_F12 && emulator) {
#ifndef __ANDROID__
        release_all_buttons(*emulator);
        debugger.toggle(sdl.window);
#endif
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_F11) {
        context.fullscreen = !context.fullscreen;
        if (!SDL_SetWindowFullscreen(sdl.window, context.fullscreen)) {
            context.fullscreen = !context.fullscreen;
            if (context.report_error) context.report_error(SDL_GetError());
        }
    } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.key == SDLK_F1) {
        if (core) release_all_buttons(*core);
        if (context.show_help) context.show_help();
    } else if (event.key.key == SDLK_ESCAPE &&
               event.type == SDL_EVENT_KEY_DOWN) {
#ifndef __ANDROID__
        if (update_download_active) {
            update_cancel_requested = true;
            return;
        }
#endif
        if (context.confirm_exit && context.confirm_exit()) context.running = false;
    } else if (core && !event.key.repeat) {
        if (const auto button = keyboard_button(bindings, event.key.key)) {
#ifndef __ANDROID__
            if (emulator &&
                supports(core.get(), CoreCapability::debugger)) {
                input_movie.set_button(*emulator, *button,
                                       event.type == SDL_EVENT_KEY_DOWN);
            } else {
                core->set_input(core_input_id(*button),
                                event.type == SDL_EVENT_KEY_DOWN);
            }
#else
            core->set_input(core_input_id(*button),
                            event.type == SDL_EVENT_KEY_DOWN);
#endif
        }
        if (link_emulator != nullptr) {
            if (const auto button = local_link_keyboard_button(event.key.key)) {
                link_emulator->set_button(*button,
                                          event.type == SDL_EVENT_KEY_DOWN);
            }
        }
    }
}

#ifdef __ANDROID__
void handle_touch_event(const SDL_Event& event, SdlEventContext& context) {
    auto& sdl = context.sdl;
    const auto finger = event.tfinger.fingerID;
    const auto [touch_x, touch_y] = window_touch_position(event.tfinger);
    if (context.dashboard_visible) {
        if (event.type == SDL_EVENT_FINGER_UP) {
            const auto [logical_x, logical_y] =
                logical_touch_position(event.tfinger, sdl);
            if (context.dashboard_row_at) {
                if (const auto selected = context.dashboard_row_at(
                        logical_x * gameboy::Ppu::screen_width,
                        logical_y * gameboy::Ppu::screen_height)) {
                    context.dashboard_selection = *selected;
                    if (context.activate_dashboard) {
                        context.activate_dashboard(*selected);
                    }
                }
            }
        }
        return;
    }

    const auto existing = std::find_if(
        sdl.touches.begin(), sdl.touches.end(),
        [finger](const SdlResources::TouchPoint& point) {
            return point.id == finger;
        });
    if (event.type == SDL_EVENT_FINGER_UP ||
        event.type == SDL_EVENT_FINGER_CANCELED) {
        if (existing != sdl.touches.end()) sdl.touches.erase(existing);
    } else if (existing == sdl.touches.end()) {
        const auto menu_tap = android_menu_touch_hit(sdl, touch_x, touch_y);
        const auto control = touch_button_index(touch_x, touch_y, sdl);
        const auto orbit = context.emulator != nullptr &&
                           supports(context.core.get(),
                                    CoreCapability::scene_layers) &&
                           !menu_tap &&
                           voxel_mode_enabled(sdl) &&
                           sdl.touch_settings.voxel_orbit && !control;
        sdl.touches.push_back(
            {finger, touch_x, touch_y, orbit, orbit ? std::nullopt : control});
    } else {
        if (event.type == SDL_EVENT_FINGER_MOTION && existing->orbit &&
            context.emulator != nullptr &&
            supports(context.core.get(), CoreCapability::scene_layers) &&
            voxel_mode_enabled(sdl) &&
            sdl.touch_settings.voxel_orbit) {
            int width = 1;
            int height = 1;
            static_cast<void>(SDL_GetWindowSize(sdl.window, &width, &height));
            const auto delta_x = (touch_x - existing->x) *
                                 static_cast<float>(width);
            const auto delta_y = (touch_y - existing->y) *
                                 static_cast<float>(height);
            sdl.voxel_camera_pitch_offset = std::clamp(
                sdl.voxel_camera_pitch_offset - delta_y * 0.25F,
                -75.0F, 75.0F);
            sdl.voxel_camera_yaw_offset = std::clamp(
                sdl.voxel_camera_yaw_offset + delta_x * 0.25F,
                -voxel_camera_yaw_drag_limit, voxel_camera_yaw_drag_limit);
        } else if (event.type == SDL_EVENT_FINGER_MOTION && !existing->orbit) {
            existing->control = gbb::retain_touch_control(
                existing->control, touch_button_index(touch_x, touch_y, sdl));
        }
        existing->x = touch_x;
        existing->y = touch_y;
    }
    if (event.type == SDL_EVENT_FINGER_DOWN &&
        android_menu_touch_hit(sdl, touch_x, touch_y)) {
        clear_touch_buttons(context.core.get(), sdl);
        if (context.open_library) context.open_library();
    }
    refresh_touch_buttons(context.core.get(), sdl);
}
#endif
#endif // GBB_EVENT_DISPATCH_CORE_ONLY

} // namespace gbb::sdl
