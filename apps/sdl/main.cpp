#include "gameboy/emulator.hpp"
#include "gameboy/gameboy_link_endpoint.hpp"
#include "gameboy/display_palette.hpp"
#include "gameboy/gameshark.hpp"
#include "gameboy/link_session.hpp"
#include "gameboy/rom_library.hpp"
#include "gameboy/tcp_link_channel.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"
#include "gameboy/video_pipeline.hpp"
#include "gbb/core_runtime.hpp"
#include "gbb/gameboy_core.hpp"
#include "gbb/frontend_logging.hpp"
#include "gbb/dashboard_navigation.hpp"
#include "gbb/gameboy_scene.hpp"
#include "gbb/settings.hpp"
#include "gbb/touch_control.hpp"
#include "gbb/video.hpp"
#include "gbb/voxel_profile.hpp"
#include "voxel_renderer.hpp"
#include "frame_presenter.hpp"
#include "frame_pacer.hpp"
#include "event_dispatch.hpp"
#include "event_policy.hpp"
#include "emulation_session.hpp"
#include "dialogs.hpp"
#include "dashboard_controller.hpp"
#include "sdl_resources.hpp"
#include "desktop_storage.hpp"
#include "window_event.hpp"
#include "tool_window_support.hpp"
#include "input_mapping.hpp"
#include "android_touch_input.hpp"
#include "input_configuration.hpp"
#include "input_lifecycle.hpp"
#include "core_capability.hpp"
#include "tas_editor.hpp"
#include "sprite_editor.hpp"
#include "cheat_manager.hpp"
#include "desktop_debugger.hpp"
#ifdef __ANDROID__
#include "android_bridge.hpp"
#endif
#include "input_movie.hpp"
#include "remote_link_session.hpp"
#include "settings_model.hpp"
#include "settings_persistence.hpp"
#ifndef __ANDROID__
#include "update_checker.hpp"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifdef GBB_HAS_SDL_TTF
#include <SDL3_ttf/SDL_ttf.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "resource.h"
#include "windows_dashboard.hpp"
#include "windows_menu_bar.hpp"
#endif

#ifdef __ANDROID__
#include <jni.h>
#endif

namespace {

using RemoteLinkSession = gbb::sdl::RemoteLinkSession;
using gbb::sdl::process_events;
using gbb::sdl::configure_video_pipeline;
using gbb::sdl::restore_video_presentation;
using gbb::sdl::load_rom;
using gbb::sdl::update_window_title;
using gbb::sdl::choose_video_mode;
using gbb::sdl::choose_display_palette;
using gbb::sdl::confirm_exit;
using gbb::sdl::show_help;
using gbb::sdl::show_about;
using gbb::sdl::show_error;
using gbb::sdl::dashboard_text;
using gbb::sdl::dashboard_items;
using gbb::sdl::dashboard_first_visible;
using gbb::sdl::dashboard_row_at;
using gbb::sdl::activate_dashboard_selection;
using gbb::sdl::dashboard_visible_rows;
using gbb::sdl::dashboard_first_row_y;
using gbb::sdl::dashboard_row_height;
#ifndef __ANDROID__
using InputMovie = gbb::sdl::InputMovie;
using gbb::sdl::start_link_trace;
using gbb::sdl::stop_link_trace;
using gbb::sdl::trace_link_frame;
using gbb::sdl::trace_remote_frame;
using gbb::sdl::start_local_link_session;
using gbb::sdl::stop_local_link_session;
using gbb::sdl::retry_local_link_session;
using gbb::sdl::start_remote_link_session;
using gbb::sdl::stop_remote_link_session;
using gbb::sdl::retry_remote_link_session;
#endif
using gbb::sdl::VoxelRenderContext;
using gbb::sdl::render_voxel_diorama;
using gbb::sdl::FrameRenderContext;
using gbb::sdl::present_link_frames;
using gbb::sdl::present_link_status;
using gbb::sdl::present_remote_link_status;
using gbb::sdl::colorize_frame;
using gbb::sdl::DialogState;
using gbb::sdl::confirm_discard_changes;
using gbb::sdl::draw_tool_button_background;
using gbb::sdl::button_order;
using gbb::sdl::gamepad_button;
using gbb::sdl::keyboard_button;
using gbb::sdl::local_link_keyboard_button;
using gbb::sdl::release_all_buttons;
using gbb::sdl::reserved_gameplay_key;
using gbb::sdl::shortcut_pressed;
using gbb::sdl::BindingConfiguration;
using gbb::sdl::BindingDevice;
using gbb::sdl::ControlsAction;
using gbb::sdl::begin_binding_configuration;
using gbb::sdl::show_controls_dialog;
using gbb::sdl::flush_battery_safely;
using gbb::sdl::stop_rumble;
using gbb::sdl::update_rumble;
#ifdef __ANDROID__
using gbb::sdl::android_menu_button_hit;
using gbb::sdl::android_menu_touch_hit;
using gbb::sdl::clear_touch_buttons;
using gbb::sdl::logical_touch_position;
using gbb::sdl::refresh_touch_buttons;
using gbb::sdl::refresh_touch_settings;
using gbb::sdl::refresh_touch_settings_if_changed;
using gbb::sdl::touch_button_index;
using gbb::sdl::voxel_mode_enabled;
using gbb::sdl::window_touch_position;
#endif
#ifndef __ANDROID__
using gbb::sdl::TasEditor;
using gbb::sdl::SpriteEditor;
using gbb::sdl::CheatManager;
using gbb::sdl::DesktopDebugger;
#endif
using gbb::sdl::load_rom_library;
using gbb::sdl::load_quick_state;
using gbb::sdl::preference_directory;
using gbb::sdl::quick_state_path;
using gbb::sdl::recent_paths;
using gbb::sdl::restore_game_window_geometry;
using gbb::sdl::save_completed_prints;
using gbb::sdl::save_game_window_geometry;
using gbb::sdl::save_quick_state;
using gbb::sdl::pump_events;
using gbb::sdl::SdlEventContext;
using gbb::sdl::handle_gamepad_device_event;
using gbb::sdl::handle_gamepad_event;
using gbb::sdl::handle_window_lifecycle_event;
using gbb::sdl::handle_mouse_event;
using gbb::sdl::handle_dashboard_key_event;
using gbb::sdl::handle_keyboard_binding_event;
using gbb::sdl::handle_gameplay_key_event;
#ifndef __ANDROID__
using gbb::sdl::handle_desktop_tool_event;
using gbb::sdl::handle_desktop_voxel_mouse_event;
#endif
#ifdef _WIN32
using gbb::sdl::handle_desktop_menu_event;
#endif

#ifndef GBB_VERSION
#define GBB_VERSION "0.0.0-dev"
#endif

[[noreturn]] void sdl_error(const std::string& action) {
    throw std::runtime_error(action + ": " + SDL_GetError());
}

using SdlResources = gbb::sdl::SdlResources;

/*
    Resource construction and teardown live in sdl_resources.cpp.  Keeping
    this alias lets the event loop retain its small, readable `sdl.foo`
    access pattern while the ownership boundary stays in one module.
*/

#ifndef __ANDROID__
// SDL_RenderDebugText is useful for the Game Boy overlay, but its fixed
// 8-pixel debug glyphs make the desktop tools hard to read. Use SDL_ttf for
// tool windows when available and retain the debug renderer as a fallback for
// minimal builds that do not ship the optional font library.
void render_tool_text(SDL_Renderer* renderer, const float x, const float y,
                      const char* value) {
#ifdef GBB_HAS_SDL_TTF
    static std::once_flag initialized;
    static TTF_Font* font = nullptr;
    std::call_once(initialized, [] {
        if (!TTF_Init()) return;
        constexpr std::array<const char*, 5> candidates{
            "fonts/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "C:/Windows/Fonts/segoeui.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/System/Library/Fonts/SFNS.ttf"};
        for (const auto* candidate : candidates) {
            font = TTF_OpenFont(candidate, 14.0F);
            if (font != nullptr) break;
        }
    });
    if (font != nullptr && value != nullptr && *value != '\0') {
        SDL_Color color{255, 255, 255, 255};
        static_cast<void>(SDL_GetRenderDrawColor(renderer, &color.r, &color.g,
                                                  &color.b, &color.a));
        auto* surface = TTF_RenderText_Blended(font, value, 0, color);
        if (surface != nullptr) {
            auto* texture = SDL_CreateTextureFromSurface(renderer, surface);
            if (texture != nullptr) {
                static_cast<void>(SDL_SetTextureBlendMode(
                    texture, SDL_BLENDMODE_BLEND));
                const SDL_FRect destination{x, y,
                                            static_cast<float>(surface->w),
                                            static_cast<float>(surface->h)};
                static_cast<void>(SDL_RenderTexture(renderer, texture, nullptr,
                                                     &destination));
                SDL_DestroyTexture(texture);
            }
            SDL_DestroySurface(surface);
            return;
        }
    }
#endif
    static_cast<void>(SDL_RenderDebugText(renderer, x, y, value));
}

#define SDL_RenderDebugText render_tool_text


#endif

#undef SDL_RenderDebugText


constexpr std::size_t maximum_rewind_frames = 180;
constexpr unsigned fast_forward_factor = 4;
using RewindHistory = std::deque<std::vector<std::uint8_t>>;



#ifdef __ANDROID__
void refresh_video_mode_if_changed(
    SdlResources& sdl, const std::filesystem::path& preference_path) {
    std::error_code error;
    const auto settings_path = portable_settings_path(preference_path);
    const auto write_time = std::filesystem::last_write_time(settings_path,
                                                               error);
    if (error) return;
    if (sdl.video_settings_write_time_valid &&
        write_time == sdl.video_settings_write_time) {
        return;
    }

    const auto mode = load_video_mode(preference_path);
    if (mode != sdl.video_mode && !configure_video_pipeline(sdl, mode)) {
        sdl_error("Could not apply the selected video pipeline");
        return;
    }
    sdl.video_settings_write_time = write_time;
    sdl.video_settings_write_time_valid = true;
}

void refresh_display_palette_if_changed(
    gbb::EmulatorCore* core, gameboy::Emulator* link_emulator,
    SdlResources& sdl, const std::filesystem::path& preference_path,
    std::size_t& display_palette) {
    std::error_code error;
    const auto settings_path = portable_settings_path(preference_path);
    const auto write_time = std::filesystem::last_write_time(settings_path,
                                                               error);
    if (error) return;
    const auto apply_palette = [&]() {
        const auto compatibility =
            gameboy::display_palettes[display_palette].cgb_compatibility;
        if (core && gbb::sdl::supports(
                        core, gbb::CoreCapability::compatibility_palette)) {
            core->set_compatibility_colors(compatibility);
        }
        if (link_emulator) {
            link_emulator->set_dmg_compatibility_colors(compatibility);
        }
    };
    if (sdl.palette_settings_write_time_valid &&
        write_time == sdl.palette_settings_write_time) {
        // Save-state loads restore PPU timing and registers but intentionally
        // do not serialize the presentation palette. Reapply the selected
        // palette even when settings.ini itself has not changed.
        apply_palette();
        return;
    }

    const auto palette = load_display_palette(preference_path);
    if (palette < gameboy::display_palettes.size() &&
        palette != display_palette) {
        display_palette = palette;
    }
    apply_palette();
    sdl.palette_settings_write_time = write_time;
    sdl.palette_settings_write_time_valid = true;
}


#endif
#ifdef __ANDROID__
void open_android_library(bool return_to_game = true) noexcept;
void leave_android_game(
    std::unique_ptr<gbb::EmulatorCore>& core,
    gameboy::Emulator*& emulator, SdlResources& sdl,
    bool& dashboard_visible, bool& paused, bool& fast_forward, bool& rewind,
    RewindHistory& rewind_history, bool& running);
#endif



#ifndef __ANDROID__
bool offer_update(const gbb_desktop::UpdateInfo& update,
                  gameboy::Emulator* emulator, SdlResources& sdl) {
    stop_rumble(sdl);
    if (emulator != nullptr) release_all_buttons(*emulator);
    const auto message =
        std::string("Go Bigger Boy ") + update.version +
        " is available.\n\nYou are running version " GBB_VERSION
        ". Would you like GBB to install the update and restart?\n\n"
        "You can keep playing while the verified archive downloads.";
    constexpr std::array<SDL_MessageBoxButtonData, 2> buttons{{
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Update now"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Later"},
    }};
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_INFORMATION,
        sdl.window,
        "Go Bigger Boy update available",
        message.c_str(),
        static_cast<int>(buttons.size()),
        buttons.data(),
        nullptr,
    };
    auto selection = 0;
    return SDL_ShowMessageBox(&box, &selection) && selection == 1;
}

std::pair<std::filesystem::path, std::filesystem::path> installation_paths() {
    const auto* base = SDL_GetBasePath();
    if (base == nullptr) throw std::runtime_error(SDL_GetError());
    auto executable_directory = std::filesystem::u8path(base).lexically_normal();
#ifdef _WIN32
    const auto executable = executable_directory / "gbb.exe";
    return {executable_directory, executable};
#elif defined(__APPLE__)
    const auto bundle = executable_directory.parent_path().parent_path();
    const auto executable = bundle / "Contents" / "MacOS" /
                            "Go Bigger Boy";
    return {bundle.parent_path(), executable};
#else
    const auto executable = executable_directory / "gbb";
    const auto root = executable_directory.filename() == "bin"
                          ? executable_directory.parent_path()
                          : executable_directory;
    return {root, executable};
#endif
}

bool installation_is_writable(const std::filesystem::path& root) {
    const auto probe = root / ".gbb-update-write-test";
    std::ofstream output(probe, std::ios::trunc);
    if (!output) return false;
    output << "write test";
    output.close();
    std::error_code ignored;
    std::filesystem::remove(probe, ignored);
    return !ignored;
}
#endif

#ifdef __ANDROID__
void open_android_library(const bool return_to_game) noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) return;
    const auto activity_class = environment->GetObjectClass(activity);
    if (activity_class != nullptr) {
        const auto method = environment->GetMethodID(
            activity_class, "openLibrary", "(Z)V");
        if (method != nullptr) {
            environment->CallVoidMethod(activity, method,
                                         static_cast<jboolean>(return_to_game));
        }
        environment->DeleteLocalRef(activity_class);
    }
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    environment->DeleteLocalRef(activity);
}

void leave_android_game(
    std::unique_ptr<gbb::EmulatorCore>& core,
    gameboy::Emulator*& emulator, SdlResources& sdl,
    bool& dashboard_visible, bool& paused, bool& fast_forward, bool& rewind,
    RewindHistory& rewind_history, bool& running) {
    if (core == nullptr) {
        if (confirm_exit(sdl.window)) running = false;
        return;
    }
    clear_touch_buttons(core.get(), sdl);
    flush_battery_safely(core.get());
    if (!confirm_exit(sdl.window)) return;

    // Keep SDL's native thread alive while the Android library is shown.
    // SDLActivity cannot start a second native main loop after the first one
    // returns, so ending it here makes the next ROM launch freeze.
    if (emulator != nullptr) release_all_buttons(*emulator);
    stop_rumble(sdl);
    sdl.camera.close();
    core.reset();
    emulator = nullptr;
    rewind_history.clear();
    fast_forward = false;
    rewind = false;
    // Android uses LibraryActivity for its dashboard. Keep the SDL surface
    // blank while it is in the background instead of showing the legacy
    // pixel-art menu.
    dashboard_visible = false;
    paused = true;
    open_android_library(false);
}

#endif

#ifdef __ANDROID__
std::string persist_android_rom(const std::string& source,
                                const std::filesystem::path& preference_path,
                                const std::string& preferred_display_name) {
    if (preference_path.empty()) return source;
    const auto rom_directory = (preference_path / "roms").lexically_normal();
    const auto source_path = std::filesystem::u8path(source).lexically_normal();
    if (source_path.parent_path() == rom_directory) return source;

    std::size_t byte_count{};
    void* loaded = SDL_LoadFile(source.c_str(), &byte_count);
    if (loaded == nullptr) {
        throw std::runtime_error(std::string{"Could not import ROM: "} +
                                 SDL_GetError());
    }
    const std::unique_ptr<void, decltype(&SDL_free)> owned(loaded, SDL_free);
    const auto* bytes = static_cast<const std::uint8_t*>(loaded);
    std::uint64_t fingerprint = 14695981039346656037ULL;
    for (std::size_t index = 0; index < byte_count; ++index) {
        fingerprint ^= bytes[index];
        fingerprint *= 1099511628211ULL;
    }

    auto display_name = preferred_display_name.empty()
                            ? source
                            : preferred_display_name;
    if (const auto query = display_name.find_first_of("?#");
        query != std::string::npos) {
        display_name.resize(query);
    }
    // Android document URIs percent-encode the user-facing filename. Retain
    // that name so metadata and Libretro artwork matching survive the import.
    std::string decoded_name;
    decoded_name.reserve(display_name.size());
    for (std::size_t index = 0; index < display_name.size(); ++index) {
        if (display_name[index] == '%' && index + 2 < display_name.size() &&
            std::isxdigit(static_cast<unsigned char>(display_name[index + 1])) &&
            std::isxdigit(static_cast<unsigned char>(display_name[index + 2]))) {
            const auto digit = [](const char value) {
                if (value >= '0' && value <= '9') return value - '0';
                return std::tolower(static_cast<unsigned char>(value)) - 'a' + 10;
            };
            decoded_name.push_back(static_cast<char>(
                digit(display_name[index + 1]) * 16 +
                digit(display_name[index + 2])));
            index += 2;
        } else {
            decoded_name.push_back(display_name[index]);
        }
    }
    display_name = std::move(decoded_name);
    if (const auto separator = display_name.find_last_of("/\\:");
        separator != std::string::npos) {
        display_name.erase(0, separator + 1);
    }
    for (auto& character : display_name) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32 || std::string_view{"/\\:*?\"<>|"}.find(character) !=
                             std::string_view::npos) {
            character = '_';
        }
    }
    if (display_name.empty()) display_name = "game.gb";
    if (display_name.size() > 160) display_name.resize(160);

    std::filesystem::create_directories(rom_directory);
    std::ostringstream filename;
    filename << std::hex << std::setw(16) << std::setfill('0') << fingerprint
             << '-' << display_name;
    const auto destination = rom_directory / filename.str();
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes),
                 static_cast<std::streamsize>(byte_count));
    if (!output) throw std::runtime_error("Could not retain the imported ROM");
    return destination.u8string();
}
#endif

void present_menu_button(SdlResources& sdl) {
#ifdef __ANDROID__
    // Unlike the Game Boy framebuffer and touch controls, this overlay belongs
    // to the full Android window. Draw it after temporarily disabling SDL's
    // logical 160x144 presentation so either corner remains reachable.
    if (!SDL_SetRenderLogicalPresentation(
            sdl.renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED)) {
        sdl_error("Could not prepare Android menu button");
    }
    const auto button_rect = android_menu_button_rect(sdl);
    const auto button_x = button_rect.x;
    const auto button_y = button_rect.y;
    const auto button_width = button_rect.w;
    const auto button_height = button_rect.h;
#else
    const auto button_x = 3.0F;
    const auto button_y = 3.0F;
    const auto button_width = 20.0F;
    const auto button_height = 15.0F;
#endif
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_BLEND));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 220, 235, 220, 190));
    const SDL_FRect button{button_x, button_y, button_width, button_height};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &button));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 16, 20, 16, 220));
    static_cast<void>(SDL_RenderRect(sdl.renderer, &button));
    const std::array<SDL_FRect, 3> menu_lines{{
        {button_x + button_width * 0.2F,
         button_y + button_height * 0.2F,
         button_width * 0.6F, button_height * 0.10F},
        {button_x + button_width * 0.2F,
         button_y + button_height * 0.47F,
         button_width * 0.6F, button_height * 0.10F},
        {button_x + button_width * 0.2F,
         button_y + button_height * 0.74F,
         button_width * 0.6F, button_height * 0.10F}}};
    for (const auto& line : menu_lines) {
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &line));
    }
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_NONE));
#ifdef __ANDROID__
    if (!restore_video_presentation(sdl)) {
        sdl_error("Could not restore game presentation after menu button");
    }
#endif
}

#ifdef __ANDROID__
void draw_touch_circle(SDL_Renderer* renderer, const float center_x,
                       const float center_y, const float radius,
                       const SDL_Color color) {
    static_cast<void>(SDL_SetRenderDrawColor(renderer, color.r, color.g,
                                             color.b, color.a));
    const auto top = static_cast<int>(std::ceil(center_y - radius));
    const auto bottom = static_cast<int>(std::floor(center_y + radius));
    for (auto y = top; y <= bottom; ++y) {
        const auto dy = static_cast<float>(y) - center_y;
        const auto span = std::sqrt(std::max(0.0F, radius * radius - dy * dy));
        const SDL_FRect row{center_x - span, static_cast<float>(y), span * 2.0F,
                            1.0F};
        static_cast<void>(SDL_RenderFillRect(renderer, &row));
    }
}

void draw_touch_label(SDL_Renderer* renderer, const float center_x,
                      const float center_y, const char* label) {
    if (label == nullptr) return;
    const auto length = static_cast<float>(std::char_traits<char>::length(label));
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 248, 252, 255, 235));
    static_cast<void>(SDL_RenderDebugText(renderer, center_x - length * 4.0F,
                                          center_y - 4.0F, label));
}

void present_touch_controls(SdlResources& sdl) {
    const auto scale = std::clamp(sdl.touch_settings.scale,
                                  minimum_touch_scale, maximum_touch_scale);
    const auto opacity = std::clamp(sdl.touch_settings.opacity,
                                    minimum_touch_opacity,
                                    maximum_touch_opacity);
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_BLEND));
    int window_width = 1;
    int window_height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &window_width,
                                        &window_height));
    const auto game_scale = touch_game_scale(sdl);
    const auto size = game_scale * scale;
    if (!SDL_SetRenderLogicalPresentation(
            sdl.renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED)) {
        sdl_error("Could not prepare touch controls");
    }
    const auto alpha = static_cast<std::uint8_t>(std::clamp(
        opacity * 255.0F, 0.0F, 255.0F));
    const auto draw_circle_button = [&](const SDL_FPoint point, const float radius,
                                        const bool pressed, const char* label) {
        // A small offset shadow separates controls from bright game scenes;
        // the pressed state uses a warm accent and a slightly smaller face,
        // giving immediate visual feedback without waiting for animation.
        draw_touch_circle(sdl.renderer, point.x + 2.0F, point.y + 3.0F,
                          radius + 2.0F, SDL_Color{0, 0, 0, 95});
        const auto face = pressed ? SDL_Color{244, 173, 67, alpha}
                                  : SDL_Color{44, 93, 143, alpha};
        draw_touch_circle(sdl.renderer, point.x, point.y,
                          radius - (pressed ? 1.5F : 0.0F), face);
        draw_touch_circle(sdl.renderer, point.x, point.y - radius * 0.18F,
                          radius * 0.72F, pressed
                              ? SDL_Color{255, 211, 124, 75}
                              : SDL_Color{126, 181, 224, 72});
        draw_touch_label(sdl.renderer, point.x, point.y, label);
    };

    const auto point_for = [&sdl, window_width, window_height](
                               const std::size_t control) {
        const auto [x, y] = touch_control_position(sdl, control);
        return SDL_FPoint{x * static_cast<float>(window_width),
                          y * static_cast<float>(window_height)};
    };
    const auto dpad = point_for(0);
    const auto dpad_arm = 21.0F * size;
    const auto dpad_thickness = 14.0F * size;
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 0, 0, 0, 95));
    const SDL_FRect dpad_shadow{dpad.x - dpad_arm + 2.0F,
                                dpad.y - dpad_thickness * 0.5F + 3.0F,
                                dpad_arm * 2.0F, dpad_thickness};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &dpad_shadow));
    const SDL_FRect dpad_vertical_shadow{dpad.x - dpad_thickness * 0.5F + 2.0F,
                                         dpad.y - dpad_arm + 3.0F,
                                         dpad_thickness, dpad_arm * 2.0F};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &dpad_vertical_shadow));
    const auto draw_dpad_arm = [&](const SDL_FRect& rect, const bool pressed) {
        static_cast<void>(SDL_SetRenderDrawColor(
            sdl.renderer, pressed ? 244 : 44, pressed ? 173 : 93,
            pressed ? 67 : 143, alpha));
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &rect));
        static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 126, 181, 224,
                                                 pressed ? 210 : 130));
        static_cast<void>(SDL_RenderRect(sdl.renderer, &rect));
    };
    draw_dpad_arm(SDL_FRect{dpad.x, dpad.y - dpad_thickness * 0.5F, dpad_arm,
                            dpad_thickness}, sdl.touch_buttons[0]);
    draw_dpad_arm(SDL_FRect{dpad.x - dpad_arm, dpad.y - dpad_thickness * 0.5F,
                            dpad_arm, dpad_thickness}, sdl.touch_buttons[1]);
    draw_dpad_arm(SDL_FRect{dpad.x - dpad_thickness * 0.5F, dpad.y - dpad_arm,
                            dpad_thickness, dpad_arm}, sdl.touch_buttons[2]);
    draw_dpad_arm(SDL_FRect{dpad.x - dpad_thickness * 0.5F, dpad.y,
                            dpad_thickness, dpad_arm}, sdl.touch_buttons[3]);
    draw_touch_circle(sdl.renderer, dpad.x, dpad.y, dpad_thickness * 0.56F,
                      SDL_Color{28, 63, 98, alpha});
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 210, 235, 250, 210));
    const auto chevron = [&](const float x1, const float y1, const float x2,
                             const float y2, const float x3, const float y3) {
        static_cast<void>(SDL_RenderLine(sdl.renderer, x1, y1, x2, y2));
        static_cast<void>(SDL_RenderLine(sdl.renderer, x2, y2, x3, y3));
    };
    const auto arrow = dpad_thickness * 0.28F;
    chevron(dpad.x - arrow, dpad.y - dpad_arm * 0.62F + arrow,
            dpad.x, dpad.y - dpad_arm * 0.62F,
            dpad.x + arrow, dpad.y - dpad_arm * 0.62F + arrow);
    chevron(dpad.x - arrow, dpad.y + dpad_arm * 0.62F - arrow,
            dpad.x, dpad.y + dpad_arm * 0.62F,
            dpad.x + arrow, dpad.y + dpad_arm * 0.62F - arrow);
    chevron(dpad.x - dpad_arm * 0.62F + arrow, dpad.y - arrow,
            dpad.x - dpad_arm * 0.62F, dpad.y,
            dpad.x - dpad_arm * 0.62F + arrow, dpad.y + arrow);
    chevron(dpad.x + dpad_arm * 0.62F - arrow, dpad.y - arrow,
            dpad.x + dpad_arm * 0.62F, dpad.y,
            dpad.x + dpad_arm * 0.62F - arrow, dpad.y + arrow);
    const std::array<float, 4> radii{{12.0F, 12.0F, 11.0F, 11.0F}};
    const std::array<const char*, 4> labels{{"A", "B", "SEL", "START"}};
    for (std::size_t control = 1; control < touch_control_count; ++control) {
        const auto point = point_for(control);
        const auto radius = radii[control - 1] * size;
        draw_circle_button(point, radius, sdl.touch_buttons[control + 3],
                           labels[control - 1]);
    }
    if (!restore_video_presentation(sdl)) {
        sdl_error("Could not restore game presentation");
    }
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_NONE));
}
#endif

#if !defined(_WIN32) && !defined(__ANDROID__)
void present_dashboard(SdlResources& sdl,
                       const std::vector<std::string>& recent,
                       const bool can_resume, std::size_t& selection) {
    const auto items = dashboard_items(can_resume, recent);
    selection = std::min(selection, items.size() - 1);
    const auto first = dashboard_first_visible(selection, items.size());
    const auto visible = std::min(dashboard_visible_rows, items.size() - first);

    // Keep the SDL dashboard visually aligned with the native desktop
    // dashboard: a deep navy canvas, bright cyan accents, and high-contrast
    // cards.  The logical 160x144 layout is intentionally compact so it
    // scales cleanly on small windows and on the Android renderer too.
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 8, 12, 20, 255));
    const SDL_FRect canvas{0, 0, 160, 144};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &canvas));
    const SDL_FRect header{0, 0, 160, 34};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &header));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 69, 207, 238, 255));
    const SDL_FRect accent{9, 31, 142, 2};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &accent));
    static_cast<void>(SDL_RenderDebugText(sdl.renderer, 13, 5,
                                          "GO BIGGER BOY"));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 177, 192, 208, 255));
    static_cast<void>(SDL_RenderDebugText(sdl.renderer, 13, 18,
                                          "GAME LIBRARY"));

    for (std::size_t row = 0; row < visible; ++row) {
        const auto index = first + row;
        const auto selected = index == selection;
        const auto y = dashboard_first_row_y +
                       static_cast<float>(row) * dashboard_row_height;
        const SDL_FRect card{9, y, 142, 15};
        static_cast<void>(SDL_SetRenderDrawColor(
            sdl.renderer, selected ? 20 : 20, selected ? 77 : 29,
            selected ? 101 : 42, 255));
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &card));
        static_cast<void>(SDL_SetRenderDrawColor(
            sdl.renderer, selected ? 230 : 137, selected ? 249 : 160,
            selected ? 255 : 183, 255));
        static_cast<void>(SDL_RenderRect(sdl.renderer, &card));
        const auto label = std::string(selected ? "> " : "  ") +
                           dashboard_text(items[index].label, 14);
        static_cast<void>(SDL_RenderDebugText(sdl.renderer, 13, y + 3,
                                              label.c_str()));
    }

    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 137, 160, 183, 255));
    if (first > 0) {
        static_cast<void>(SDL_RenderDebugText(sdl.renderer, 153, 39, "^"));
    }
    if (first + visible < items.size()) {
        static_cast<void>(SDL_RenderDebugText(sdl.renderer, 153, 111, "v"));
    }
    static_cast<void>(SDL_RenderDebugText(sdl.renderer, 13, 134,
                                          "UP/DOWN  ENTER SELECT"));
}
#endif

void present(const gbb::EmulatorCore* core,
             const gameboy::Emulator* emulator,
             const gameboy::Emulator* link_emulator, SdlResources& sdl,
             const gameboy::LinkSession* link_session,
             const RemoteLinkSession* remote_link,
             const gameboy::DisplayPalette& palette,
             const std::vector<std::string>& recent,
             const bool dashboard_visible,
             std::size_t& dashboard_selection) {
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 16, 20, 16, 255));
    if (!SDL_RenderClear(sdl.renderer)) {
        sdl_error("Could not clear framebuffer");
    }
    if (dashboard_visible) {
#if !defined(_WIN32) && !defined(__ANDROID__)
        present_dashboard(sdl, recent, core != nullptr,
                          dashboard_selection);
#else
        static_cast<void>(recent);
        static_cast<void>(dashboard_selection);
#endif
    } else if (core != nullptr && emulator != nullptr &&
               gbb::sdl::supports(core, gbb::CoreCapability::link_cable) &&
               link_emulator != nullptr) {
        FrameRenderContext frame_context{
            sdl.renderer, sdl.texture, sdl.link_texture, sdl.video_mode};
        if (!present_link_frames(*emulator, *link_emulator, frame_context,
                                 palette)) {
            sdl_error("Could not present linked framebuffers");
        }
        if (link_session != nullptr &&
            !present_link_status(frame_context, *link_session)) {
            sdl_error("Could not present link status");
        }
    } else if (core != nullptr) {
        if (emulator != nullptr &&
            gbb::sdl::supports(core, gbb::CoreCapability::scene_layers) &&
            (sdl.video_mode == gameboy::VideoMode::voxel_diorama ||
            sdl.video_mode == gameboy::VideoMode::voxel_shape ||
            sdl.video_mode == gameboy::VideoMode::voxel_popup)) {
            VoxelRenderContext voxel_context{
                sdl.renderer,
                sdl.texture,
                sdl.video_mode,
                sdl.scene_snapshot,
                sdl.voxel_profile_path,
                sdl.voxel_profile,
                sdl.voxel_profile_fingerprint,
                sdl.voxel_profile_loaded,
                sdl.voxel_vertices,
                sdl.voxel_indices,
                sdl.voxel_camera_pitch_offset,
                sdl.voxel_camera_yaw_offset};
            if (!render_voxel_diorama(
                    *emulator, voxel_context, palette,
                    sdl.video_mode == gameboy::VideoMode::voxel_shape,
                    sdl.video_mode == gameboy::VideoMode::voxel_popup)) {
                sdl_error("Could not render voxel diorama");
            }
        } else {
        FrameRenderContext frame_context{
            sdl.renderer, sdl.texture, sdl.link_texture, sdl.video_mode};
        const auto colored_pixels = colorize_frame(*core, frame_context, palette);
        const auto frame = core->video_frame();
        if (!SDL_UpdateTexture(sdl.texture, nullptr, colored_pixels.data(),
                               static_cast<int>(frame.width *
                                                sizeof(std::uint32_t))) ||
            !SDL_RenderTexture(sdl.renderer, sdl.texture, nullptr, nullptr)) {
            sdl_error("Could not present framebuffer");
        }
        }
        if (remote_link != nullptr && remote_link->active()) {
            FrameRenderContext frame_context{
                sdl.renderer, sdl.texture, sdl.link_texture, sdl.video_mode};
            if (!present_remote_link_status(frame_context, *remote_link)) {
                sdl_error("Could not present remote link status");
            }
        }
    }
#ifdef __ANDROID__
    if (!dashboard_visible && core != nullptr) {
        present_touch_controls(sdl);
    }
#endif
#ifndef _WIN32
    if (!dashboard_visible && core != nullptr) present_menu_button(sdl);
#endif
    if (!SDL_RenderPresent(sdl.renderer)) {
        sdl_error("Could not present framebuffer");
    }
}

} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << "Go Bigger Boy " GBB_VERSION << '\n';
            return EXIT_SUCCESS;
        }
        DialogState dialog;
#ifdef _WIN32
        const auto start_with_library = argc != 2;
#else
        constexpr auto start_with_library = false;
#endif
        SdlResources sdl(GBB_VERSION, start_with_library);
#ifdef _WIN32
        DesktopMenuBar desktop_menu;
        desktop_menu.attach(sdl.window);
#endif
        const auto preference_path = preference_directory();
        sdl.voxel_profile_path = preference_path.empty()
                                    ? std::filesystem::path{}
                                    : preference_path / "voxel-profiles.ini";
        gbb::ensure_voxel_profile_file(sdl.voxel_profile_path);
        restore_game_window_geometry(sdl.window, preference_path);
        auto rom_library = load_rom_library(preference_path);
        auto recent_roms = recent_paths(rom_library);
        auto bindings = load_bindings(preference_path);
        auto configuration_backup = bindings;
        auto display_palette = load_display_palette(preference_path);
        const auto link_diagnostics = load_link_diagnostics(preference_path);
        auto video_mode = load_video_mode(preference_path);
        if (!configure_video_pipeline(sdl, video_mode)) {
            sdl_error("Could not configure video pipeline");
        }
#ifdef __ANDROID__
        const auto touch_settings = load_touch_control_settings(preference_path);
        sdl.touch_settings = touch_settings;
#endif
        std::unique_ptr<gbb::EmulatorCore> core;
        gameboy::Emulator* emulator = nullptr;
        std::unique_ptr<gameboy::Emulator> link_emulator;
        std::unique_ptr<gameboy::LinkSession> link_session;
        std::unique_ptr<gameboy::GameBoyLinkEndpoint> link_first_endpoint;
        std::unique_ptr<gameboy::GameBoyLinkEndpoint> link_second_endpoint;
        RemoteLinkSession remote_link;
        std::string current_rom;
        std::optional<std::string> pending_rom;
#ifdef __ANDROID__
        std::string pending_rom_name;
#endif
        std::optional<BindingConfiguration> configuring;
#ifndef __ANDROID__
        gbb_desktop::UpdateChecker update_checker{GBB_VERSION};
        std::optional<gbb_desktop::UpdateInfo> available_update;
        std::unique_ptr<gbb_desktop::UpdateDownload> update_download;
        bool update_cancel_requested = false;
        bool update_check_complete = false;
        std::optional<bool> cheat_pause_restore;
#endif
#ifdef _WIN32
        bool reveal_sdl_after_present = false;
#endif
        auto paused = false;
        auto fullscreen = false;
        auto fast_forward = false;
        auto rewind = false;
        auto reset_requested = false;
        auto link_toggle_requested = false;
        auto link_retry_requested = false;
        auto automatic_local_retry_used = false;
        auto remote_host_requested = false;
        auto remote_join_requested = false;
        auto remote_stop_requested = false;
        auto running = true;
#ifdef __ANDROID__
        // The native Android LibraryActivity owns the dashboard. The SDL
        // surface must never render the legacy pixel-art dashboard.
        auto dashboard_visible = false;
#else
        auto dashboard_visible = argc != 2;
#endif
#ifdef _WIN32
        if (dashboard_visible) SDL_HideWindow(sdl.window);
#endif
        std::size_t dashboard_selection = 0;
        // ROMs selected in the native dashboard wait for the update check to
        // finish so an available update can be offered before the ROM boots.
        bool pending_rom_from_dashboard = false;
        std::uint64_t print_sequence = 0;
        RewindHistory rewind_history;
#ifndef __ANDROID__
        DesktopDebugger debugger;
        InputMovie input_movie;
        TasEditor tas_editor;
        SpriteEditor sprite_editor;
        CheatManager cheat_manager;
        const auto movie_path = preference_path / "replays" /
                                "last-input.gbbmovie";
        const auto sprite_patch_path = preference_path / "sprite-patches" /
                                       "last-sprite-edit.gbbtiles";
        const auto sprite_ips_path = preference_path / "sprite-patches" /
                                     "last-sprite-edit.ips";
#endif

#ifdef __ANDROID__
        if (argc >= 2) {
            pending_rom = argv[1];
            if (argc >= 3) pending_rom_name = argv[2];
        } else {
#else
        if (argc == 2) {
            pending_rom = argv[1];
        } else {
            if (argc > 2) {
                show_error(sdl.window, "Only one ROM can be opened at a time.");
            }
#endif
            static_cast<void>(SDL_SetWindowTitle(
                sdl.window, "Go Bigger Boy (GBB) - Game Library"));
        }
        auto cycles_per_frame = 70224U;
        gbb::sdl::FramePacer frame_pacer(cycles_per_frame);
        std::uint64_t frontend_frame = 0;

        while (running) {
            // Make the current presentation frame available to every nested
            // diagnostic without threading context through each SDL helper.
            gbb::LogContextScope frame_log_context(
                {0, frontend_frame, 0,
                 core ? core->rom_fingerprint() : 0});
#ifdef __ANDROID__
            gbb::sdl::publish_android_log_context(
                gbb::current_log_context());
            // Settings are edited by the native Android activity while this
            // SDL activity may stay alive underneath it. Poll before event
            // handling and rendering so the video pipeline, menu placement,
            // and voxel orbit preferences take effect immediately, even
            // without a touch.
            refresh_video_mode_if_changed(sdl, preference_path);
            refresh_display_palette_if_changed(
                core.get(), link_emulator.get(), sdl, preference_path,
                display_palette);
            refresh_touch_settings_if_changed(sdl, preference_path);
#endif
#ifndef __ANDROID__
            if (!update_check_complete) {
                std::string update_error;
                std::optional<gbb_desktop::UpdateInfo> update_result;
                gbb::LogContext update_context{};
                if (update_checker.take_result(update_result, update_error,
                                               &update_context)) {
                    auto callback_context =
                        gbb::LogContextScope::exact(update_context);
                    update_check_complete = true;
                    if (!update_error.empty()) {
                        gbb::log_frontend_warning(
                            std::string("Update check unavailable: ") +
                            update_error);
                    }
                    available_update = std::move(update_result);
                }
            }
#endif
#ifdef _WIN32
            if (dashboard_visible && !update_download) {
                save_game_window_geometry(sdl.window, preference_path);
                SDL_HideWindow(sdl.window);
                gbb_desktop::KeyboardBindings dashboard_bindings{};
                for (std::size_t index = 0; index < bindings.keys.size();
                     ++index) {
                    for (std::size_t slot = 0;
                         slot < bindings.keys[index].size(); ++slot) {
                        dashboard_bindings[index][slot] =
                            static_cast<std::int64_t>(bindings.keys[index][slot]);
                    }
                }
                gbb_desktop::ActionBindings dashboard_actions{};
                for (std::size_t index = 0; index < dashboard_actions.size();
                     ++index) {
                    dashboard_actions[index] = static_cast<std::int64_t>(
                        bindings.shortcuts[index]);
                }
                const auto result = gbb_desktop::show_windows_dashboard(
                    nullptr, rom_library, core != nullptr,
                    core ? core->rom_fingerprint() : 0,
                    core ? core->descriptor().capabilities
                         : gbb::CoreCapability::none,
                    display_palette,
                    sdl.video_mode,
                    dashboard_bindings, dashboard_actions,
                    preference_path);
                if (!result.removed_fingerprints.empty()) {
                    for (const auto fingerprint : result.removed_fingerprints) {
                        static_cast<void>(rom_library.remove(fingerprint));
                    }
                    rom_library.save(preference_path);
                    recent_roms = recent_paths(rom_library);
                }
                if (result.palette_changed &&
                    result.palette < gameboy::display_palettes.size()) {
                    display_palette = result.palette;
                    save_display_palette(preference_path, display_palette);
                    if (core && gbb::sdl::supports(
                                   core.get(),
                                   gbb::CoreCapability::compatibility_palette)) {
                        core->set_compatibility_colors(
                            gameboy::display_palettes[display_palette]
                                .cgb_compatibility);
                    }
                }
                if (result.video_mode_changed) {
                    sdl.video_mode = result.video_mode;
                    save_video_mode(preference_path, sdl.video_mode);
                    if (!configure_video_pipeline(sdl, sdl.video_mode)) {
                        show_error(sdl.window,
                                   "Could not configure the selected video pipeline.");
                    }
                }
                if (result.voxel_profile_changed) {
                    sdl.voxel_profile_loaded = false;
                }
                if (result.keyboard_bindings_changed) {
                    for (std::size_t index = 0; index < bindings.keys.size();
                         ++index) {
                        for (std::size_t slot = 0;
                             slot < bindings.keys[index].size(); ++slot) {
                            bindings.keys[index][slot] =
                                static_cast<SDL_Keycode>(
                                    result.keyboard_bindings[index][slot]);
                        }
                    }
                    save_bindings(preference_path, bindings);
                }
                if (result.action_bindings_changed) {
                    for (std::size_t index = 0;
                         index < bindings.shortcuts.size(); ++index) {
                        bindings.shortcuts[index] = static_cast<SDL_Keycode>(
                            result.action_bindings[index]);
                    }
                    save_bindings(preference_path, bindings);
                }
                switch (result.action) {
                case gbb_desktop::DashboardResultAction::open_rom:
                    pending_rom = result.rom_path;
                    pending_rom_from_dashboard = true;
                    dashboard_visible = false;
                    break;
                case gbb_desktop::DashboardResultAction::resume:
                    dashboard_visible = false;
                    reveal_sdl_after_present = true;
                    break;
                case gbb_desktop::DashboardResultAction::quit:
                    running = false;
                    break;
                }
                if (!running) break;
            }
#endif
#ifdef __ANDROID__
            if (auto requested = gbb::sdl::take_android_rom_request()) {
                auto callback_context =
                    gbb::LogContextScope::exact(requested->log_context);
                pending_rom = std::move(requested->path);
                pending_rom_name = std::move(requested->display_name);
                gbb::log_frontend_info("Android ROM open request accepted");
            }
#endif
            SdlEventContext event_context{
                core,
                emulator,
                link_emulator.get(),
                sdl,
                dialog,
                preference_path,
                bindings,
                configuration_backup,
                recent_roms,
                current_rom,
                configuring,
                pending_rom,
                display_palette,
                dashboard_visible,
                dashboard_selection,
                paused,
                fullscreen,
                fast_forward,
                rewind,
                rewind_history,
                reset_requested,
                link_toggle_requested,
                link_retry_requested,
                remote_host_requested,
                remote_join_requested,
                remote_stop_requested,
                remote_link.active(),
                running
#ifndef __ANDROID__
                , update_download != nullptr,
                update_cancel_requested,
                cheat_pause_restore,
                debugger,
                input_movie,
                tas_editor,
                sprite_editor,
                cheat_manager
#ifdef _WIN32
                , desktop_menu
#endif
#endif
                , [&]() {
                    return dashboard_items(core != nullptr, recent_roms).size();
                }
                , [&](const float x, const float y) {
                    return dashboard_row_at(
                        x, y, dashboard_selection,
                        dashboard_items(core != nullptr, recent_roms).size());
                }
                , [&](const std::size_t selection) {
                    activate_dashboard_selection(
                        selection, recent_roms, bindings, core.get(), dialog,
                        sdl, preference_path, pending_rom, dashboard_visible,
                        display_palette, running);
                }
                , [&]() {
                    if (core) release_all_buttons(*core);
#ifdef __ANDROID__
                    open_android_library();
#else
#ifdef _WIN32
                    SDL_HideWindow(sdl.window);
#endif
                    dashboard_visible = true;
                    dashboard_selection = 0;
#endif
                }
                , [&]() { show_help(sdl.window, bindings); }
                , [&]() { show_rom_dialog(dialog, sdl.window); }
                , [&]() {
                    choose_display_palette(core.get(), sdl, preference_path,
                                           display_palette);
                }
                , [&]() { return confirm_exit(sdl.window); }
                , [&](const std::string& message) { show_error(sdl.window, message); }
                , [&]() {
                    update_window_title(sdl.window, current_rom, paused,
                                        configuring);
                }
                , [&]() { show_about(sdl.window); }
                , [&]() {
#ifdef __ANDROID__
                    leave_android_game(core, emulator, sdl, dashboard_visible,
                                       paused, fast_forward, rewind,
                                       rewind_history, running);
#endif
                }
            };
            process_events(event_context);

#ifndef __ANDROID__
            if (update_cancel_requested) {
                update_cancel_requested = false;
                if (update_download) update_download->cancel();
            }
            if (cheat_pause_restore && !cheat_manager.visible()) {
                paused = *cheat_pause_restore;
                cheat_pause_restore.reset();
                update_window_title(sdl.window, current_rom, paused,
                                    configuring);
            }
            if (remote_stop_requested) {
                remote_stop_requested = false;
                if (emulator != nullptr && remote_link.active()) {
                    stop_remote_link_session(*emulator, remote_link);
                }
            }
            if (remote_host_requested || remote_join_requested) {
                const auto hosting = remote_host_requested;
                remote_host_requested = false;
                remote_join_requested = false;
                try {
                    if (link_emulator != nullptr) {
                        stop_local_link_session(*emulator, link_emulator,
                                                link_session, link_first_endpoint,
                                                link_second_endpoint, sdl);
                    }
                    if (remote_link.active()) {
                        stop_remote_link_session(*emulator, remote_link);
                    }
                    if (!gbb::sdl::supports(
                            core.get(), gbb::CoreCapability::link_cable) ||
                        emulator == nullptr) {
                        gbb::log_frontend_warning(
                            "Ignoring link request for a core without link support.");
                    } else {
                        start_remote_link_session(*emulator, remote_link, hosting,
                                                  preference_path, link_diagnostics,
                                                  sdl.window);
                        rewind = false;
                        rewind_history.clear();
                    }
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            if (link_retry_requested) {
                link_retry_requested = false;
                try {
                    if (emulator != nullptr && remote_link.active()) {
                        retry_remote_link_session(*emulator, remote_link);
                    } else if (emulator != nullptr && link_emulator != nullptr &&
                               link_session != nullptr) {
                        retry_local_link_session(*emulator, *link_emulator,
                                                 *link_session);
                        automatic_local_retry_used = false;
                    }
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            // A stalled local session used to leave both consoles permanently
            // frozen once LinkSession's transport watchdog expired. Recover
            // the transient serial/guest handshake once, matching the manual
            // Retry Link Handshake command, but do not loop forever if the
            // peer or ROM is genuinely unavailable.
            if (emulator != nullptr && link_emulator != nullptr &&
                link_session != nullptr &&
                link_session->state() == gameboy::LinkSession::State::timed_out &&
                !automatic_local_retry_used) {
                try {
                    retry_local_link_session(*emulator, *link_emulator,
                                             *link_session);
                    automatic_local_retry_used = true;
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                    automatic_local_retry_used = true;
                }
            }
            if (link_toggle_requested) {
                link_toggle_requested = false;
                try {
                    if (remote_link.active()) {
                        stop_remote_link_session(*emulator, remote_link);
                    } else if (link_emulator != nullptr) {
                        stop_local_link_session(*emulator, link_emulator,
                                                link_session, link_first_endpoint,
                                                link_second_endpoint, sdl);
                    } else if (emulator != nullptr &&
                               gbb::sdl::supports(
                                   core.get(), gbb::CoreCapability::link_cable)) {
                        start_local_link_session(
                            current_rom, *emulator, link_emulator, link_session,
                            link_first_endpoint, link_second_endpoint,
                            sdl, gameboy::display_palettes[display_palette],
                            preference_path, link_diagnostics);
                        automatic_local_retry_used = false;
                        rewind = false;
                        rewind_history.clear();
                    }
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            if (link_emulator == nullptr) automatic_local_retry_used = false;
#endif

            std::optional<std::string> dialog_error;
            collect_dialog_result(dialog, pending_rom, dialog_error);
            if (dialog_error) show_error(sdl.window, *dialog_error);

            if (reset_requested) {
                if (!current_rom.empty()) pending_rom = current_rom;
                reset_requested = false;
            }

#ifndef __ANDROID__
            // Handle the update offer before loading a ROM selected from the
            // dashboard.  The dashboard itself is a modal native window, so
            // this runs as soon as it returns with the user's selection.
            if (available_update && !dialog_active(dialog) && !configuring) {
                if (pending_rom_from_dashboard) {
                    SDL_ShowWindow(sdl.window);
                    SDL_RaiseWindow(sdl.window);
                }
                if (offer_update(*available_update, emulator, sdl)) {
                    try {
                        const auto [root, executable] = installation_paths();
                        if (!installation_is_writable(root)) {
                            throw std::runtime_error(
                                "The installation directory is not writable. "
                                "Install GBB in a user-writable folder to use "
                                "automatic updates.");
                        }
                        const auto directory =
                            (preference_path.empty()
                                 ? std::filesystem::temp_directory_path() /
                                       "go-bigger-boy"
                                 : preference_path) /
                            "updates" / available_update->version;
                        update_download =
                            std::make_unique<gbb_desktop::UpdateDownload>(
                                *available_update, directory);
                        SDL_ShowWindow(sdl.window);
                        SDL_RaiseWindow(sdl.window);
                        static_cast<void>(SDL_SetWindowTitle(
                            sdl.window,
                            "Go Bigger Boy (GBB) - Downloading update..."));
                    } catch (const std::exception& error) {
                        show_error(sdl.window, error.what());
                    }
                }
                available_update.reset();
                frame_pacer.reset();
            }

            if (update_download) {
                const auto downloaded_bytes = update_download->downloaded_bytes();
                const auto total_bytes = update_download->total_bytes();
                std::string title = "Go Bigger Boy (GBB) - Downloading update";
                if (total_bytes > 0) {
                    const auto percent = std::min<std::uintmax_t>(
                        100, downloaded_bytes * 100 / total_bytes);
                    title += " (" + std::to_string(percent) + "%)";
                } else {
                    title += "...";
                }
                title += " - press Escape to cancel";
                static_cast<void>(SDL_SetWindowTitle(sdl.window, title.c_str()));
                std::optional<gbb_desktop::DownloadedUpdate> downloaded;
                std::string download_error;
                gbb::LogContext update_context{};
                if (update_download->take_result(downloaded, download_error,
                                                 &update_context)) {
                    auto callback_context =
                        gbb::LogContextScope::exact(update_context);
                    const auto was_cancelled = update_download->cancelled();
                    update_download.reset();
                    if (!download_error.empty() || !downloaded) {
                        if (!was_cancelled) {
                            show_error(sdl.window,
                                       download_error.empty()
                                           ? "The update download failed."
                                           : download_error);
                        }
                        update_window_title(sdl.window, current_rom, paused,
                                            configuring);
                    } else {
                        try {
                            const auto [root, executable] = installation_paths();
                            std::string installer_error;
                            if (!gbb_desktop::launch_update_installer(
                                    *downloaded, root, executable,
                                    installer_error)) {
                                throw std::runtime_error(installer_error);
                            }
                            running = false;
                        } catch (const std::exception& error) {
                            show_error(sdl.window, error.what());
                        }
                    }
                }
            }
#endif

            if (pending_rom &&
#ifndef __ANDROID__
                (!pending_rom_from_dashboard || update_check_complete) &&
                !update_download &&
#endif
                running) {
                try {
#ifndef __ANDROID__
                    if (remote_link.active()) {
                        stop_remote_link_session(*emulator, remote_link);
                    }
                    if (link_emulator != nullptr) {
                        stop_local_link_session(*emulator, link_emulator,
                                                link_session, link_first_endpoint,
                                                link_second_endpoint, sdl);
                    }
                    input_movie.stop(emulator);
                    tas_editor.close();
                    sprite_editor.reset_session();
                    if (cheat_pause_restore) {
                        paused = *cheat_pause_restore;
                        cheat_pause_restore.reset();
                    }
                    cheat_manager.close();
#endif
                    auto requested_rom = *pending_rom;
#ifdef __ANDROID__
                    requested_rom =
                        persist_android_rom(requested_rom, preference_path,
                                            pending_rom_name);
#endif
                    const bool reopening_current =
                        emulator && requested_rom == current_rom;
                    load_rom(requested_rom, core,
                             gameboy::display_palettes[display_palette], sdl,
                             preference_path);
                    emulator = gbb::gameboy_emulator(core.get());
                    sdl.camera.close();
                    if (emulator != nullptr &&
                        gbb::sdl::supports(core.get(),
                                           gbb::CoreCapability::camera)) {
                        sdl.camera.configure(*emulator);
                    }
                    cycles_per_frame = core->descriptor().nominal_cycles_per_frame;
                    frame_pacer.set_timing(cycles_per_frame,
                                           core->descriptor().clock_rate);
                    sdl.audio.clear();
                    current_rom = requested_rom;
                    if (!reopening_current) paused = false;
                    fast_forward = false;
                    rewind = false;
                    rewind_history.clear();
                    dashboard_visible = false;
#ifdef _WIN32
                    reveal_sdl_after_present = true;
#endif
                    rom_library.remember(
                        current_rom, gameboy::inspect_rom_file(current_rom));
#ifndef __ANDROID__
                    cheat_manager.load(preference_path,
                                       gameboy::inspect_rom_file(current_rom));
#endif
                    rom_library.save(preference_path);
                    recent_roms = recent_paths(rom_library);
                    update_window_title(sdl.window, current_rom, paused,
                                        configuring);
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                    if (!emulator) {
#ifdef _WIN32
                        SDL_HideWindow(sdl.window);
#endif
                        dashboard_visible = false;
                        dashboard_selection = 0;
#ifdef __ANDROID__
                        open_android_library();
#endif
                    }
                }
                pending_rom.reset();
                pending_rom_from_dashboard = false;
#ifdef __ANDROID__
                pending_rom_name.clear();
#endif
                frame_pacer.reset();
            }

#ifndef __ANDROID__
            if (emulator &&
                gbb::sdl::supports(core.get(), gbb::CoreCapability::debugger) &&
                debugger.take_record_toggle()) {
                try {
                    if (input_movie.recording()) {
                        input_movie.stop_and_save(movie_path, *emulator);
                    } else {
                        input_movie.stop(emulator);
                        input_movie.start_recording(*emulator);
                        rewind_history.clear();
                        paused = false;
                        fast_forward = false;
                        rewind = false;
                        debugger.run();
                    }
                } catch (const std::exception& error) {
                    input_movie.stop(emulator);
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator &&
                gbb::sdl::supports(core.get(), gbb::CoreCapability::debugger) &&
                debugger.take_replay_request()) {
                try {
                    input_movie.stop(emulator);
                    input_movie.start_replay(*emulator, movie_path);
                    rewind_history.clear();
                    paused = false;
                    fast_forward = false;
                    rewind = false;
                    sdl.audio.clear();
                    debugger.run();
                } catch (const std::exception& error) {
                    input_movie.stop(emulator);
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator &&
                gbb::sdl::supports(core.get(), gbb::CoreCapability::debugger) &&
                debugger.take_tas_request()) {
                input_movie.stop(emulator);
                tas_editor.open(sdl.window, *emulator);
                rewind_history.clear();
                debugger.pause();
            }
            if (emulator &&
                gbb::sdl::supports(core.get(), gbb::CoreCapability::sprite_editor) &&
                debugger.take_sprite_request()) {
                input_movie.stop(emulator);
                sprite_editor.open(sdl.window, *emulator);
                rewind_history.clear();
                debugger.pause();
            }
            if (emulator &&
                gbb::sdl::supports(core.get(), gbb::CoreCapability::sprite_editor) &&
                sprite_editor.take_save_patch_request()) {
                try {
                    sprite_editor.save_patch(*emulator, sprite_patch_path);
                    sprite_editor.mark_saved(*emulator);
                    const auto message = "Sprite patch saved to:\n" +
                                         sprite_patch_path.string();
                    static_cast<void>(SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_INFORMATION, "Sprite patch saved",
                        message.c_str(), sdl.window));
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator &&
                gbb::sdl::supports(core.get(), gbb::CoreCapability::sprite_editor) &&
                sprite_editor.take_load_patch_request()) {
                try {
                    sprite_editor.load_patch(*emulator, sprite_patch_path);
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator &&
                gbb::sdl::supports(core.get(), gbb::CoreCapability::sprite_editor) &&
                sprite_editor.take_export_ips_request()) {
                try {
                    const auto result = sprite_editor.export_ips(
                        *emulator, current_rom, sprite_ips_path);
                    const auto message =
                        "IPS patch saved to:\n" + sprite_ips_path.string() +
                        "\n\nTiles exported: " +
                        std::to_string(result.exported) +
                        "\nTiles skipped because their ROM source was "
                        "missing or ambiguous: " +
                        std::to_string(result.unresolved);
                    static_cast<void>(SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_INFORMATION, "IPS patch exported",
                        message.c_str(), sdl.window));
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator &&
                gbb::sdl::supports(core.get(), gbb::CoreCapability::cheats) &&
                cheat_manager.take_fetch_request()) {
                cheat_manager.start_fetch();
            }
            if (cheat_manager.poll_fetch()) {
                if (const auto error = cheat_manager.take_fetch_error()) {
                    show_error(sdl.window, *error);
                }
            }
            if (emulator &&
                gbb::sdl::supports(core.get(), gbb::CoreCapability::debugger) &&
                tas_editor.take_new_request()) {
                    input_movie.stop(emulator);
                tas_editor.reset_from(*emulator);
                rewind_history.clear();
                debugger.pause();
            }
            if (emulator &&
                gbb::sdl::supports(core.get(), gbb::CoreCapability::debugger) &&
                tas_editor.take_save_request()) {
                try {
                    input_movie.stop(emulator);
                    input_movie.save_frame_inputs(
                        *emulator, movie_path, tas_editor.fingerprint(),
                        tas_editor.start_state(), tas_editor.frames());
                    static_cast<void>(emulator->take_audio_samples());
                    sdl.audio.clear();
                    tas_editor.mark_saved();
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator &&
                gbb::sdl::supports(core.get(), gbb::CoreCapability::debugger) &&
                tas_editor.take_replay_request()) {
                try {
                    input_movie.stop(emulator);
                    input_movie.save_frame_inputs(
                        *emulator, movie_path, tas_editor.fingerprint(),
                        tas_editor.start_state(), tas_editor.frames());
                    input_movie.start_replay(*emulator, movie_path);
                    static_cast<void>(emulator->take_audio_samples());
                    rewind_history.clear();
                    paused = false;
                    fast_forward = false;
                    rewind = false;
                    sdl.audio.clear();
                    debugger.run();
                } catch (const std::exception& error) {
                    input_movie.stop(emulator);
                    show_error(sdl.window, error.what());
                }
            }
#endif

            sdl.camera.update(
                gbb::sdl::supports(core.get(), gbb::CoreCapability::camera)
                    ? emulator
                    : nullptr);
#ifndef __ANDROID__
            if (remote_link.active()) remote_link.endpoint.poll();
#endif
#ifndef __ANDROID__
            if (input_movie.replaying()) {
                fast_forward = false;
                rewind = false;
            }
#endif

            bool debugger_stepped = false;
            bool replay_ended = false;
            const auto step_emulator = [&]() {
#ifndef __ANDROID__
                if (input_movie.update_replay(*emulator)) {
                    replay_ended = true;
                    return cycles_per_frame;
                }
#endif
                return emulator->step();
            };
#ifndef __ANDROID__
            if (emulator &&
                gbb::sdl::supports(core.get(), gbb::CoreCapability::debugger) &&
                debugger.take_instruction_step()) {
                static_cast<void>(step_emulator());
                if (emulator->frame_ready()) emulator->consume_frame();
                rewind_history.clear();
                debugger_stepped = true;
            } else if (emulator &&
                       gbb::sdl::supports(core.get(), gbb::CoreCapability::debugger) &&
                       debugger.take_frame_step()) {
                if (emulator->frame_ready()) emulator->consume_frame();
                cheat_manager.apply(*emulator);
                unsigned cycles = 0;
                while (cycles < cycles_per_frame * 2U &&
                       !emulator->frame_ready()) {
                    cycles += step_emulator();
                }
                if (emulator->frame_ready()) emulator->consume_frame();
                rewind_history.clear();
                debugger_stepped = true;
            }
            const auto debugger_paused = debugger.execution_paused();
#else
            constexpr auto debugger_paused = false;
#endif
            if (core && !debugger_stepped && !paused && !debugger_paused &&
                !dashboard_visible && !configuring
#ifndef __ANDROID__
                && !cheat_manager.visible() && !cheat_manager.fetching()
#endif
                && !dialog_active(dialog)) {
                if (rewind && link_emulator == nullptr) {
                    if (!rewind_history.empty()) {
                        auto state = std::move(rewind_history.back());
                        rewind_history.pop_back();
                        core->load_state(state);
                        release_all_buttons(*core);
                        sdl.audio.clear();
                    }
                } else {
                    // Rewind states describe one complete machine. During a
                    // linked session there are two machines and the history
                    // would be both incomplete and expensive to serialize;
                    // keep normal-speed multiplayer responsive by disabling
                    // rewind state capture for that session.
                    const auto frames = fast_forward ? fast_forward_factor : 1U;
                    for (auto frame = 0U; frame < frames && running; ++frame) {
#ifndef __ANDROID__
                        if (emulator &&
                            gbb::sdl::supports(core.get(),
                                               gbb::CoreCapability::cheats)) {
                            cheat_manager.apply(*emulator);
                        }
#endif
                        if (link_emulator == nullptr) {
                            rewind_history.push_back(core->save_state());
                            while (rewind_history.size() > maximum_rewind_frames) {
                                rewind_history.pop_front();
                            }
                        }
                        if (link_emulator != nullptr && link_session != nullptr) {
                            // The session owns the cable and keeps both CPU
                            // timelines balanced so serial interrupts cannot
                            // be starved by frontend scheduling.
                            link_session->advance(cycles_per_frame);
                        } else if (!remote_link.active()
#ifndef __ANDROID__
                                   && !input_movie.replaying()
#endif
                        ) {
                            // Keep the ordinary single-console path on the
                            // shared runtime contract. Link transport polling
                            // and input replay retain their specialized loops.
                            static_cast<void>(gbb::advance_to_frame(
                                *core, cycles_per_frame));
                        } else {
                            unsigned cycles = 0;
                            unsigned remote_poll_cycles = 0;
                            while (running && cycles < cycles_per_frame &&
                                   !emulator->frame_ready()) {
                                const auto stepped = step_emulator();
                                cycles += stepped;
#ifndef __ANDROID__
                                if (remote_link.active()) {
                                    remote_poll_cycles += stepped;
                                    // Keep network serial edges well below a
                                    // video-frame of latency. Polling every
                                    // 64 CPU cycles avoids the per-frame delay
                                    // that can make Pokémon's Cable Club probe
                                    // time out even on loopback.
                                    if (remote_poll_cycles >= 64) {
                                        remote_link.endpoint.poll();
                                        remote_poll_cycles = 0;
                                    }
                                }
#endif
                            }
#ifndef __ANDROID__
                            if (remote_link.active()) remote_link.endpoint.poll();
#endif
                        }
                        if (core->frame_ready()) core->consume_frame();
                        if (link_emulator != nullptr &&
                            link_emulator->frame_ready()) {
                            link_emulator->consume_frame();
                        }
#ifndef __ANDROID__
                        if (link_emulator != nullptr) {
                            const auto audio_queued_bytes = sdl.audio.queued_bytes();
                            trace_link_frame(*emulator, *link_emulator,
                                             audio_queued_bytes);
                        } else if (remote_link.active()) {
                            const auto audio_queued_bytes = sdl.audio.queued_bytes();
                            trace_remote_frame(*emulator, remote_link,
                                               audio_queued_bytes);
                        }
#endif
                    }
                }
            }
#ifndef __ANDROID__
            if (replay_ended) debugger.pause();
#endif
            update_rumble(core.get(), sdl,
                          !paused && !debugger_paused && !rewind &&
                              !dashboard_visible &&
                              !configuring &&
                              !dialog_active(dialog));
            sdl.audio.submit(core.get(), fast_forward);
            if (link_emulator != nullptr) {
                // Player two currently shares the primary audio device. Drain
                // its mixer buffer so it cannot grow stale and add latency or
                // memory pressure during long link sessions.
                static_cast<void>(link_emulator->take_audio_samples());
            }
            try {
                save_completed_prints(core.get(), sdl.window,
                                      preference_path, current_rom,
                                      print_sequence);
            } catch (const std::exception& error) {
                show_error(sdl.window, error.what());
            }
            present(core.get(), emulator, link_emulator.get(), sdl, link_session.get(),
                    remote_link.active() ? &remote_link : nullptr,
                    gameboy::display_palettes[display_palette], recent_roms,
                    dashboard_visible, dashboard_selection);
#ifndef __ANDROID__
            if (emulator) {
                debugger.present(*emulator,
                                 gameboy::display_palettes[display_palette],
                                 input_movie);
            }
            tas_editor.present();
            sprite_editor.present(emulator);
            cheat_manager.present();
#endif
#ifdef _WIN32
            if (reveal_sdl_after_present && !dashboard_visible) {
                SDL_ShowWindow(sdl.window);
                reveal_sdl_after_present = false;
            }
#endif

            frame_pacer.advance();
            ++frontend_frame;
            if (remote_link.active()) {
                frame_pacer.wait([&] { remote_link.endpoint.poll(); });
            } else {
                frame_pacer.wait();
            }
        }
#ifndef __ANDROID__
        if (emulator && input_movie.recording()) {
            try {
                input_movie.stop_and_save(movie_path, *emulator);
            } catch (const std::exception& error) {
                gbb::log_frontend_warning(
                    std::string("Could not save input recording: ") +
                    error.what());
            }
        }
#endif
        save_game_window_geometry(sdl.window, preference_path);
#ifndef __ANDROID__
        if (emulator != nullptr && link_emulator != nullptr) {
            stop_local_link_session(*emulator, link_emulator, link_session,
                                    link_first_endpoint, link_second_endpoint,
                                    sdl);
        }
        if (emulator != nullptr && remote_link.active()) {
            stop_remote_link_session(*emulator, remote_link);
        }
#endif
        flush_battery_safely(core.get());
    } catch (const std::exception& error) {
        gbb::log_frontend_error(std::string("SDL frontend error: ") +
                                error.what());
#ifdef _WIN32
        MessageBoxA(nullptr, error.what(), "Go Bigger Boy (GBB)",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
