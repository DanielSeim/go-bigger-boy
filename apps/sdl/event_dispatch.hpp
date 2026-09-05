#pragma once

#include <SDL3/SDL.h>

#include "gameboy/gameshark.hpp"
#include "desktop_storage.hpp"
#include "input_configuration.hpp"
#include "sdl_resources.hpp"
#include "gbb/core.hpp"

#ifndef __ANDROID__
#include "cheat_manager.hpp"
#include "desktop_debugger.hpp"
#include "input_movie.hpp"
#include "sprite_editor.hpp"
#include "tas_editor.hpp"
#endif

#ifdef _WIN32
#include "windows_menu_bar.hpp"
#endif

#include <deque>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gbb::sdl {

// All mutable state touched by SDL event policy is collected here. Keeping the
// aggregate in the dispatcher module gives handlers and callers one stable
// contract while the runtime loop remains responsible for emulation policy.
struct SdlEventContext {
    std::unique_ptr<gbb::EmulatorCore>& core;
    // Non-owning Game Boy adapter. The runtime owns the core-neutral session;
    // this is only populated for the built-in core's advanced tools.
    gameboy::Emulator* emulator;
    gameboy::Emulator* link_emulator;
    SdlResources& sdl;
    DialogState& dialog;
    const std::filesystem::path& preference_path;
    InputBindings& bindings;
    InputBindings& configuration_backup;
    const std::vector<std::string>& recent;
    const std::string& current_rom;
    std::optional<BindingConfiguration>& configuring;
    std::optional<std::string>& pending_rom;
    std::size_t& display_palette;
    bool& dashboard_visible;
    std::size_t& dashboard_selection;
    bool& paused;
    bool& fullscreen;
    bool& fast_forward;
    bool& rewind;
    std::deque<std::vector<std::uint8_t>>& rewind_history;
    bool& reset_requested;
    bool& link_toggle_requested;
    bool& link_retry_requested;
    bool& remote_host_requested;
    bool& remote_join_requested;
    bool& remote_discover_requested;
    bool& remote_stop_requested;
    bool remote_link_active;
    bool& running;
#ifndef __ANDROID__
    bool update_download_active;
    bool& update_cancel_requested;
    std::optional<bool>& cheat_pause_restore;
    DesktopDebugger& debugger;
    InputMovie& input_movie;
    TasEditor& tas_editor;
    SpriteEditor& sprite_editor;
    CheatManager& cheat_manager;
#ifdef _WIN32
    DesktopMenuBar& desktop_menu;
#endif
#endif
    std::function<std::size_t()> dashboard_item_count;
    std::function<std::optional<std::size_t>(float, float)> dashboard_row_at;
    std::function<void(std::size_t)> activate_dashboard;
    std::function<void()> open_library;
    std::function<void()> open_android_link_settings;
    std::function<void()> show_help;
    std::function<void()> open_rom_dialog;
    std::function<void()> choose_palette;
    std::function<bool()> confirm_exit;
    std::function<void(const std::string&)> report_error;
    std::function<void()> update_title;
    std::function<void()> show_about;
    // Runtime-specific exit handling (for example, returning to the Android
    // library without tearing down SDL's native loop).
    std::function<void()> leave_game;
};

// Owns the SDL event-pump boundary. The caller supplies the continuation
// predicate so shutdown can stop polling immediately, and the handler keeps
// all application policy on the SDL thread.
using EventPumpContinue = std::function<bool()>;
using EventHandler = std::function<void(const SDL_Event&)>;

void pump_events(const EventPumpContinue& should_continue,
                 const EventHandler& handle_event);

void handle_gamepad_device_event(const SDL_Event& event,
                                 SdlEventContext& context);

void handle_gamepad_event(const SDL_Event& event, SdlEventContext& context);

#ifdef _WIN32
void handle_desktop_menu_event(SdlEventContext& context);
#endif

void handle_window_lifecycle_event(const SDL_Event& event,
                                   SdlEventContext& context);

void handle_mouse_event(const SDL_Event& event, SdlEventContext& context);

bool handle_dashboard_key_event(const SDL_Event& event,
                                SdlEventContext& context);

bool handle_keyboard_binding_event(const SDL_Event& event,
                                   SdlEventContext& context);

void handle_gameplay_key_event(const SDL_Event& event,
                               SdlEventContext& context);

#ifndef __ANDROID__
[[nodiscard]] bool is_voxel_video_mode(gameboy::VideoMode mode) noexcept;
bool handle_desktop_tool_event(const SDL_Event& event,
                               SdlEventContext& context);
void handle_desktop_voxel_mouse_event(const SDL_Event& event,
                                      SdlEventContext& context);
#endif

#ifdef __ANDROID__
void handle_touch_event(const SDL_Event& event, SdlEventContext& context);
#endif

} // namespace gbb::sdl
