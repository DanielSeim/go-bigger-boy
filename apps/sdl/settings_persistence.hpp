#pragma once

#include "settings_model.hpp"

#include "gameboy/video_pipeline.hpp"

#include <filesystem>
#include <string>

struct AppSettings {
    InputBindings bindings;
    std::size_t palette{};
    gameboy::VideoMode video_mode{gameboy::default_video_mode};
    bool link_diagnostics{};
    TouchControlSettings touch;
};

[[nodiscard]] std::filesystem::path portable_settings_path(
    const std::filesystem::path& preference_directory);

[[nodiscard]] float parse_touch_value(const std::string& value,
                                      float fallback, float minimum,
                                      float maximum);

[[nodiscard]] bool parse_bool_setting(const std::string& value, bool fallback);

[[nodiscard]] const char* keyboard_key_setting_name(SDL_Keycode key);
[[nodiscard]] SDL_Keycode keyboard_key_from_setting(const std::string& value);
[[nodiscard]] const char* gamepad_button_setting_name(
    SDL_GamepadButton button);
[[nodiscard]] SDL_GamepadButton gamepad_button_from_setting(
    const std::string& value);

void write_portable_settings(const std::filesystem::path& preference_directory,
                             const AppSettings& settings);

void append_missing_portable_settings(
    const std::filesystem::path& path, const AppSettings& settings,
    bool has_palette, const std::array<bool, 8>& has_keyboard,
    const std::array<bool, 8>& has_gamepad,
    const std::array<bool, shortcut_names.size()>& has_shortcuts,
    bool has_video_mode, bool has_link_diagnostics, bool has_touch_scale,
    bool has_touch_opacity, bool has_touch_voxel_orbit,
    bool has_touch_menu_position,
    const std::array<bool, touch_layout_count * touch_control_count>&
        has_touch_positions);

[[nodiscard]] AppSettings load_portable_settings(
    const std::filesystem::path& preference_directory);
[[nodiscard]] AppSettings load_app_settings(
    const std::filesystem::path& preference_directory);
void save_app_settings(const std::filesystem::path& preference_directory,
                       const InputBindings& bindings, std::size_t palette);
[[nodiscard]] gameboy::VideoMode load_video_mode(
    const std::filesystem::path& directory);
[[nodiscard]] bool load_link_diagnostics(
    const std::filesystem::path& directory);
void save_video_mode(const std::filesystem::path& directory,
                     gameboy::VideoMode mode);
[[nodiscard]] TouchControlSettings load_touch_control_settings(
    const std::filesystem::path& directory);
void save_touch_control_settings(const std::filesystem::path& directory,
                                 float scale, float opacity);
void save_touch_voxel_orbit(const std::filesystem::path& directory,
                            bool enabled);
void save_touch_menu_position(const std::filesystem::path& directory,
                              bool top_right);
[[nodiscard]] std::array<float, touch_layout_count * touch_layout_stride>
load_touch_control_layout(const std::filesystem::path& directory);
void save_touch_control_layout(
    const std::filesystem::path& directory,
    const std::array<float, touch_layout_count * touch_layout_stride>&
        positions);
[[nodiscard]] InputBindings load_bindings(
    const std::filesystem::path& directory);
[[nodiscard]] std::size_t load_display_palette(
    const std::filesystem::path& directory);
void save_bindings(const std::filesystem::path& directory,
                   const InputBindings& bindings);
void save_display_palette(const std::filesystem::path& directory,
                          std::size_t palette);



[[nodiscard]] InputBindings load_legacy_bindings(
    const std::filesystem::path& directory);

#ifdef __ANDROID__
void save_legacy_bindings(const std::filesystem::path& directory,
                          const InputBindings& bindings);
#endif

[[nodiscard]] std::size_t load_legacy_display_palette(
    const std::filesystem::path& directory);
