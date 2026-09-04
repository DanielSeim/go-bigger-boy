#pragma once

#include "sdl_resources.hpp"
#include "gbb/core.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <utility>

namespace gbb::sdl {

#ifdef __ANDROID__

[[nodiscard]] bool touch_is_landscape(const SdlResources& sdl);
[[nodiscard]] std::size_t touch_layout_offset(const SdlResources& sdl);
[[nodiscard]] float touch_game_scale(const SdlResources& sdl);
[[nodiscard]] bool voxel_mode_enabled(const SdlResources& sdl);
[[nodiscard]] SDL_FRect android_menu_button_rect(const SdlResources& sdl);
[[nodiscard]] bool android_menu_touch_hit(const SdlResources& sdl, float x,
                                          float y);
[[nodiscard]] bool android_menu_button_hit(const SdlResources& sdl, float x,
                                           float y);
[[nodiscard]] std::pair<float, float> touch_control_position(
    const SdlResources& sdl, std::size_t control);
[[nodiscard]] std::optional<std::size_t> touch_button_index(
    float x, float y, const SdlResources& sdl);

void refresh_touch_buttons(gbb::EmulatorCore* core, SdlResources& sdl);
void clear_touch_buttons(gbb::EmulatorCore* core, SdlResources& sdl);
void refresh_touch_settings(SdlResources& sdl,
                            const std::filesystem::path& preference_path);
void refresh_touch_settings_if_changed(
    SdlResources& sdl, const std::filesystem::path& preference_path);
[[nodiscard]] std::pair<float, float> logical_touch_position(
    const SDL_TouchFingerEvent& event, SdlResources& sdl);
[[nodiscard]] std::pair<float, float> window_touch_position(
    const SDL_TouchFingerEvent& event);

#endif

} // namespace gbb::sdl
