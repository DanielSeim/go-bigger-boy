#pragma once

#include "gameboy/video_pipeline.hpp"
#include "input_configuration.hpp"
#include "input_mapping.hpp"
#include "sdl_resources.hpp"
#include "gameboy/lan_discovery.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gbb::sdl {

void update_window_title(
    SDL_Window* window, const std::string& current_rom, bool paused,
    const std::optional<BindingConfiguration>& configuring);

void choose_video_mode(SdlResources& sdl,
                       const std::filesystem::path& preference_path);

void choose_display_palette(gbb::EmulatorCore* core, SdlResources& sdl,
                            const std::filesystem::path& preference_path,
                            std::size_t& display_palette);

bool confirm_exit(SDL_Window* window);
void show_help(SDL_Window* window, const InputBindings& bindings);
void show_about(SDL_Window* window);
void show_error(SDL_Window* window, const std::string& message);
void show_lan_hosts(SDL_Window* window, const std::vector<gameboy::LanPeer>& peers);

#ifndef __ANDROID__
void show_desktop_notification(SDL_Window* window, std::string message,
                               bool warning = false);
[[nodiscard]] bool desktop_notification_visible(SDL_Window* window) noexcept;
void present_desktop_notification(SDL_Renderer* renderer, SDL_Window* window);
void open_desktop_controls_dialog(
    SDL_Window* window, const InputBindings& bindings,
    std::function<void(ControlsAction)> on_choice);
[[nodiscard]] bool desktop_dialog_visible(SDL_Window* window) noexcept;
bool handle_desktop_dialog_event(const SDL_Event& event);
void present_desktop_dialog(SDL_Renderer* renderer, SDL_Window* window);
#endif

} // namespace gbb::sdl
