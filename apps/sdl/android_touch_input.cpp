#include "android_touch_input.hpp"

#include "input_mapping.hpp"
#include "settings_persistence.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <system_error>

namespace gbb::sdl {

#ifdef __ANDROID__

bool touch_is_landscape(const SdlResources& sdl) {
    int width = 1;
    int height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &width, &height));
    return width >= height;
}

std::size_t touch_layout_offset(const SdlResources& sdl) {
    return (touch_is_landscape(sdl) ? 1U : 0U) * touch_layout_stride;
}

float touch_game_scale(const SdlResources& sdl) {
    int width = 1;
    int height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &width, &height));
    return std::min(static_cast<float>(width) /
                        static_cast<float>(sdl.core_video_width),
                    static_cast<float>(height) /
                        static_cast<float>(sdl.core_video_height));
}

bool voxel_mode_enabled(const SdlResources& sdl) {
    return sdl.video_mode == gameboy::VideoMode::voxel_diorama ||
           sdl.video_mode == gameboy::VideoMode::voxel_shape ||
           sdl.video_mode == gameboy::VideoMode::voxel_popup;
}

SDL_FRect android_menu_button_rect(const SdlResources& sdl) {
    int width = 1;
    int height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &width, &height));
    // The game framebuffer is normally rendered through a 160x144 logical
    // viewport. The menu is an Android overlay, however, so size and position
    // it in full-window pixels after disabling logical presentation.
    const auto size = touch_game_scale(sdl) * 20.0F;
    const auto button_height = size * 0.75F;
    const auto margin = std::max(8.0F, size * 0.15F);
    const auto x = sdl.touch_settings.menu_top_right
                       ? std::max(margin, static_cast<float>(width) - size - margin)
                       : margin;
    return {x, margin, size, button_height};
}

SDL_FRect android_link_button_rect(const SdlResources& sdl) {
    const auto menu = android_menu_button_rect(sdl);
    const auto gap = std::max(6.0F, menu.w * 0.2F);
    const auto x = sdl.touch_settings.menu_top_right
                       ? menu.x - gap - menu.w
                       : menu.x + menu.w + gap;
    return {x, menu.y, menu.w, menu.h};
}

bool android_menu_touch_hit(const SdlResources& sdl, const float x,
                            const float y) {
    int width = 1;
    int height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &width, &height));
    const auto button = android_menu_button_rect(sdl);
    const auto pixel_x = x * static_cast<float>(width);
    const auto pixel_y = y * static_cast<float>(height);
    constexpr float hit_slop = 8.0F;
    return pixel_x >= button.x - hit_slop &&
           pixel_x <= button.x + button.w + hit_slop &&
           pixel_y >= button.y - hit_slop &&
           pixel_y <= button.y + button.h + hit_slop;
}

bool android_link_touch_hit(const SdlResources& sdl, const float x,
                            const float y) {
    int width = 1;
    int height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &width, &height));
    const auto button = android_link_button_rect(sdl);
    const auto pixel_x = x * static_cast<float>(width);
    const auto pixel_y = y * static_cast<float>(height);
    constexpr float hit_slop = 8.0F;
    return pixel_x >= button.x - hit_slop &&
           pixel_x <= button.x + button.w + hit_slop &&
           pixel_y >= button.y - hit_slop &&
           pixel_y <= button.y + button.h + hit_slop;
}

bool android_menu_button_hit(const SdlResources& sdl, const float x,
                             const float y) {
    const auto button = android_menu_button_rect(sdl);
    constexpr float hit_slop = 8.0F;
    return x >= button.x - hit_slop &&
           x <= button.x + button.w + hit_slop &&
           y >= button.y - hit_slop &&
           y <= button.y + button.h + hit_slop;
}

std::pair<float, float> touch_control_position(const SdlResources& sdl,
                                               const std::size_t control) {
    const auto index = touch_layout_offset(sdl) + control * 2;
    return {sdl.touch_settings.positions[index],
            sdl.touch_settings.positions[index + 1]};
}

std::optional<std::size_t> touch_button_index(const float x, const float y,
                                              const SdlResources& sdl) {
    const auto scale = std::clamp(sdl.touch_settings.scale,
                                  minimum_touch_scale, maximum_touch_scale);
    int width = 1;
    int height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &width, &height));
    const auto pixel_x = x * static_cast<float>(width);
    const auto pixel_y = y * static_cast<float>(height);
    const auto size = touch_game_scale(sdl) * scale;
    constexpr std::array<float, 4> widths{{42.0F, 24.0F, 24.0F, 22.0F}};
    constexpr std::array<float, 4> heights{{42.0F, 24.0F, 24.0F, 10.0F}};
    const auto inside = [&](const std::size_t control) {
        const auto [normalized_x, normalized_y] =
            touch_control_position(sdl, control);
        const auto center_x = normalized_x * static_cast<float>(width);
        const auto center_y = normalized_y * static_cast<float>(height);
        const auto dx = pixel_x - center_x;
        const auto dy = pixel_y - center_y;
        if (control == 0) {
            return std::abs(dx) <= widths[0] * size * 0.5F &&
                   std::abs(dy) <= heights[0] * size * 0.5F;
        }
        if (control == 1 || control == 2) {
            const auto radius = widths[control] * size * 0.5F;
            return dx * dx + dy * dy <= radius * radius;
        }
        // Face/system controls are stored in the arrays starting at index
        // zero (A, B, Select, Start), while their control IDs start at one.
        // Using `control` directly made Start read past both arrays and gave
        // it an undefined (usually non-interactive) touch hitbox.
        const auto face_index = control - 1;
        return std::abs(dx) <= widths[face_index] * size * 0.5F &&
               std::abs(dy) <= heights[face_index] * size * 0.5F;
    };
    // Check the face and system buttons before the D-pad if a custom layout
    // intentionally places controls near one another.
    for (const auto control : {std::size_t{1}, std::size_t{2}, std::size_t{3},
                               std::size_t{4}}) {
        if (inside(control)) return control + 3;
    }
    if (inside(0)) {
        const auto [normalized_x, normalized_y] = touch_control_position(sdl, 0);
        const auto dx = pixel_x - normalized_x * static_cast<float>(width);
        const auto dy = pixel_y - normalized_y * static_cast<float>(height);
        return std::abs(dx) >= std::abs(dy) ? (dx >= 0.0F ? 0U : 1U)
                                            : (dy >= 0.0F ? 3U : 2U);
    }
    return std::nullopt;
}

void refresh_touch_buttons(gbb::EmulatorCore* core, SdlResources& sdl) {
    std::array<bool, 8> pressed{};
    for (const auto& touch : sdl.touches) {
        if (touch.orbit) continue;
        if (touch.control && *touch.control < pressed.size()) {
            pressed[*touch.control] = true;
        }
    }
    if (core != nullptr) {
        for (std::size_t index = 0; index < pressed.size(); ++index) {
            if (pressed[index] != sdl.touch_buttons[index]) {
                core->set_input(core_input_id(button_order[index]),
                                pressed[index]);
            }
        }
    }
    sdl.touch_buttons = pressed;
}

void clear_touch_buttons(gbb::EmulatorCore* core, SdlResources& sdl) {
    sdl.touches.clear();
    if (core != nullptr) {
        for (std::size_t index = 0; index < sdl.touch_buttons.size(); ++index) {
            if (sdl.touch_buttons[index]) {
                core->set_input(core_input_id(button_order[index]), false);
            }
        }
    }
    sdl.touch_buttons.fill(false);
}

void refresh_touch_settings(SdlResources& sdl,
                            const std::filesystem::path& preference_path) {
    sdl.touch_settings = load_touch_control_settings(preference_path);
    if (!sdl.touch_settings.voxel_orbit) {
        // If settings changed while a gesture was in progress, do not let an
        // old per-touch orbit flag bypass the newly disabled preference.
        for (auto& touch : sdl.touches) touch.orbit = false;
    }
    std::error_code error;
    const auto settings_path = portable_settings_path(preference_path);
    const auto write_time = std::filesystem::last_write_time(settings_path,
                                                               error);
    if (!error) {
        sdl.touch_settings_write_time = write_time;
        sdl.touch_settings_write_time_valid = true;
    }
}

void refresh_touch_settings_if_changed(
    SdlResources& sdl, const std::filesystem::path& preference_path) {
    std::error_code error;
    const auto settings_path = portable_settings_path(preference_path);
    const auto write_time = std::filesystem::last_write_time(settings_path,
                                                               error);
    if (error) return;
    if (!sdl.touch_settings_write_time_valid ||
        write_time != sdl.touch_settings_write_time) {
        refresh_touch_settings(sdl, preference_path);
    }
}
std::pair<float, float> logical_touch_position(const SDL_TouchFingerEvent& event,
                                                SdlResources& sdl) {
    int width = 1;
    int height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &width, &height));
    auto x = event.x * static_cast<float>(width);
    auto y = event.y * static_cast<float>(height);
    static_cast<void>(
        SDL_RenderCoordinatesFromWindow(sdl.renderer, x, y, &x, &y));
    return {x / static_cast<float>(sdl.core_video_width),
            y / static_cast<float>(sdl.core_video_height)};
}

std::pair<float, float> window_touch_position(
    const SDL_TouchFingerEvent& event) {
    return {event.x, event.y};
}

#endif

} // namespace gbb::sdl
