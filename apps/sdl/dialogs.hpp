#pragma once

#include "gameboy/video_pipeline.hpp"
#include "input_configuration.hpp"
#include "input_mapping.hpp"
#include "sdl_resources.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

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

} // namespace gbb::sdl
