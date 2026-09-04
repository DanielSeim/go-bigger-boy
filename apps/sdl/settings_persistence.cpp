#include "settings_persistence.hpp"

#include "gbb/frontend_logging.hpp"

#include "gameboy/display_palette.hpp"
#include "gbb/settings.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

const char* keyboard_key_setting_name(const SDL_Keycode key) {
    if (key == SDLK_UNKNOWN) return "None";
    if (key == SDLK_LSHIFT) return "Left Shift";
    if (key == SDLK_GRAVE) return "Grave";
    return SDL_GetKeyName(key);
}

SDL_Keycode keyboard_key_from_setting(const std::string& value) {
    // Keep human-readable names stable across SDL versions and preserve the
    // legacy Grave shortcut for existing settings files.
    if (value == "Left Shift" || value == "LShift") return SDLK_LSHIFT;
    if (value == "Grave" || value == "Backquote") return SDLK_GRAVE;
    return SDL_GetKeyFromName(value.c_str());
}

const char* gamepad_button_setting_name(const SDL_GamepadButton button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "south";
    case SDL_GAMEPAD_BUTTON_EAST: return "east";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "dpad_right";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "dpad_left";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "dpad_up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "dpad_down";
    default: return SDL_GetGamepadStringForButton(button);
    }
}

SDL_GamepadButton gamepad_button_from_setting(const std::string& value) {
    if (value == "south") return SDL_GAMEPAD_BUTTON_SOUTH;
    if (value == "east") return SDL_GAMEPAD_BUTTON_EAST;
    if (value == "dpad_right") return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    if (value == "dpad_left") return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    if (value == "dpad_up") return SDL_GAMEPAD_BUTTON_DPAD_UP;
    if (value == "dpad_down") return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    return SDL_GetGamepadButtonFromString(value.c_str());
}

void append_missing_portable_settings(
    const std::filesystem::path& path, const AppSettings& settings,
    const bool has_palette, const std::array<bool, 8>& has_keyboard,
    const std::array<bool, 8>& has_gamepad,
    const std::array<bool, shortcut_names.size()>& has_shortcuts,
    const bool has_video_mode, const bool has_link_diagnostics,
    const bool has_touch_scale, const bool has_touch_opacity,
    const bool has_touch_voxel_orbit, const bool has_touch_menu_position,
    const std::array<bool, touch_layout_count * touch_control_count>&
        has_touch_positions) {
    const auto complete = has_palette &&
        std::all_of(has_keyboard.begin(), has_keyboard.end(),
                    [](const bool value) { return value; }) &&
        std::all_of(has_gamepad.begin(), has_gamepad.end(),
                    [](const bool value) { return value; }) &&
        std::all_of(has_shortcuts.begin(), has_shortcuts.end(),
                    [](const bool value) { return value; }) &&
        has_video_mode && has_link_diagnostics && has_touch_scale &&
        has_touch_opacity && has_touch_voxel_orbit && has_touch_menu_position &&
        std::all_of(has_touch_positions.begin(), has_touch_positions.end(),
                    [](const bool value) { return value; });
    if (complete) return;
    std::ofstream output(path, std::ios::app);
    if (!output) {
        gbb::log_frontend_warning(
            std::string("Could not complete portable settings file: ") +
            path.string());
        return;
    }
    output << "\n# Missing entries added automatically by GBB\n";
    if (!has_palette) {
        output << "palette = "
               << gameboy::display_palettes[settings.palette].id << '\n';
    }
    if (!has_video_mode) {
        output << "video.Mode = "
               << gameboy::video_mode_info(settings.video_mode).id << '\n';
    }
    if (!has_link_diagnostics) {
        output << "link.Diagnostics = "
               << (settings.link_diagnostics ? "true" : "false") << '\n';
    }
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        if (has_keyboard[index]) continue;
        output << "keyboard." << button_names[index] << " = "
               << keyboard_key_setting_name(settings.bindings.keys[index][0]);
        if (settings.bindings.keys[index][1] != SDLK_UNKNOWN) {
            output << ' '
                   << keyboard_key_setting_name(settings.bindings.keys[index][1]);
        }
        output << '\n';
    }
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        if (has_gamepad[index]) continue;
        output << "gamepad." << button_names[index] << " = "
               << gamepad_button_setting_name(
                      settings.bindings.gamepad_buttons[index])
               << '\n';
    }
    for (std::size_t index = 0; index < shortcut_names.size(); ++index) {
        if (has_shortcuts[index]) continue;
        output << "keyboard." << shortcut_names[index] << " = "
               << keyboard_key_setting_name(settings.bindings.shortcuts[index])
               << '\n';
    }
    if (!has_touch_scale) output << "touch.Size = " << settings.touch.scale << '\n';
    if (!has_touch_opacity) {
        output << "touch.Opacity = " << settings.touch.opacity << '\n';
    }
    if (!has_touch_voxel_orbit) {
        output << "touch.VoxelOrbit = "
               << (settings.touch.voxel_orbit ? "true" : "false") << '\n';
    }
    if (!has_touch_menu_position) {
        output << "touch.MenuPosition = "
               << (settings.touch.menu_top_right ? "top-right" : "top-left")
               << '\n';
    }
    for (std::size_t orientation = 0; orientation < touch_layout_count;
         ++orientation) {
        for (std::size_t control = 0; control < touch_control_count;
             ++control) {
            const auto index = orientation * touch_layout_stride + control * 2;
            const auto entry = orientation * touch_control_count + control;
            if (has_touch_positions[entry]) continue;
            output << "touch." << touch_layout_names[orientation] << '.'
                   << touch_control_names[control] << " = "
                   << settings.touch.positions[index] << ' '
                   << settings.touch.positions[index + 1] << '\n';
        }
    }
}

void write_portable_settings(const std::filesystem::path& preference_directory,
                             const AppSettings& settings) {
    const auto path = portable_settings_path(preference_directory);
    if (path.empty()) return;
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        gbb::log_frontend_warning(
            std::string("Could not write portable settings file: ") +
            path.string());
        return;
    }
    output << "# Go Bigger Boy portable settings\n"
              "# Copy this file beside another GBB installation to share "
              "these settings.\n"
              "# Add an optional second keyboard key after the first, for "
              "example: Z Y. Set emulator shortcuts to None to disable "
              "them. Android touch controls use a size multiplier and "
              "opacity between 0 and 1 plus separate portrait and landscape "
              "touch layouts. Touch.VoxelOrbit controls touch camera gestures "
              "in voxel modes and Touch.MenuPosition accepts top-left or "
              "top-right. Video.Mode accepts nearest, bilinear, sharp, "
              "integer, lcd, voxel, voxel_shape, or voxel_popup. "
              "Link.Diagnostics enables the opt-in link "
              "serial, CPU, and game-state trace.\n\n"
              "palette = "
           << gameboy::display_palettes[settings.palette].id << "\n"
              "video.Mode = "
           << gameboy::video_mode_info(settings.video_mode).id << "\n"
              "link.Diagnostics = "
           << (settings.link_diagnostics ? "true" : "false") << "\n\n";
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        output << "keyboard." << button_names[index] << " = "
               << keyboard_key_setting_name(settings.bindings.keys[index][0]);
        if (settings.bindings.keys[index][1] != SDLK_UNKNOWN) {
            output << ' '
                   << keyboard_key_setting_name(settings.bindings.keys[index][1]);
        }
        output << '\n';
    }
    output << '\n';
    for (std::size_t index = 0; index < shortcut_names.size(); ++index) {
        output << "keyboard." << shortcut_names[index] << " = "
               << keyboard_key_setting_name(settings.bindings.shortcuts[index])
               << '\n';
    }
    output << '\n';
    output << "touch.Size = " << settings.touch.scale << '\n';
    output << "touch.Opacity = " << settings.touch.opacity << '\n';
    output << "touch.VoxelOrbit = "
           << (settings.touch.voxel_orbit ? "true" : "false") << '\n';
    output << "touch.MenuPosition = "
           << (settings.touch.menu_top_right ? "top-right" : "top-left")
           << "\n\n";
    for (std::size_t orientation = 0; orientation < touch_layout_count;
         ++orientation) {
        for (std::size_t control = 0; control < touch_control_count;
             ++control) {
            const auto index = orientation * touch_layout_stride + control * 2;
            output << "touch." << touch_layout_names[orientation] << '.'
                   << touch_control_names[control] << " = "
                   << settings.touch.positions[index] << ' '
                   << settings.touch.positions[index + 1] << '\n';
        }
    }
    output << '\n';
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        output << "gamepad." << button_names[index] << " = "
               << gamepad_button_setting_name(
                      settings.bindings.gamepad_buttons[index])
               << '\n';
    }
}


float parse_touch_value(const std::string& value, const float fallback,
                        const float minimum, const float maximum) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stof(value, &consumed);
        if (consumed != value.size()) return fallback;
        return std::clamp(parsed, minimum, maximum);
    } catch (const std::exception&) {
        return fallback;
    }
}

bool parse_bool_setting(const std::string& value, const bool fallback) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (normalized == "true" || normalized == "yes" || normalized == "on" ||
        normalized == "1") {
        return true;
    }
    if (normalized == "false" || normalized == "no" || normalized == "off" ||
        normalized == "0") {
        return false;
    }
    return fallback;
}

std::filesystem::path portable_settings_path(
    const std::filesystem::path& preference_directory) {
#ifdef __ANDROID__
    return preference_directory / "settings.ini";
#else
    const auto* raw_base = SDL_GetBasePath();
    if (raw_base == nullptr) return {};
    auto directory = std::filesystem::u8path(raw_base).lexically_normal();
    if (directory.filename().empty()) directory = directory.parent_path();
#ifdef __APPLE__
    directory = directory.parent_path().parent_path().parent_path();
#elif !defined(_WIN32)
    if (directory.filename() == "bin") directory = directory.parent_path();
#endif
    return directory / "settings.ini";
#endif
}

InputBindings load_legacy_bindings(const std::filesystem::path& directory) {
    InputBindings bindings;
    if (directory.empty()) return bindings;
    std::ifstream input(directory / "controls.txt");
    auto loaded = bindings.keys;
    for (auto& keys : loaded) {
        long long value = 0;
        if (!(input >> value)) return bindings;
        keys[0] = static_cast<SDL_Keycode>(value);
    }
    std::array<SDL_Keycode, 8> unique_keys{};
    std::transform(loaded.begin(), loaded.end(), unique_keys.begin(),
                   [](const auto& keys) { return keys[0]; });
    if (std::find(unique_keys.begin(), unique_keys.end(), SDLK_UNKNOWN) !=
        unique_keys.end()) return bindings;
    std::sort(unique_keys.begin(), unique_keys.end());
    if (std::adjacent_find(unique_keys.begin(), unique_keys.end()) !=
        unique_keys.end()) {
        return bindings;
    }
    bindings.keys = loaded;

    // Older controls files contain only the eight keyboard bindings.
    auto loaded_gamepad = bindings.gamepad_buttons;
    for (auto& button : loaded_gamepad) {
        int value = 0;
        if (!(input >> value)) return bindings;
        if (value < 0 || value >= SDL_GAMEPAD_BUTTON_COUNT) return bindings;
        button = static_cast<SDL_GamepadButton>(value);
    }
    auto unique_buttons = loaded_gamepad;
    std::sort(unique_buttons.begin(), unique_buttons.end());
    if (std::adjacent_find(unique_buttons.begin(), unique_buttons.end()) !=
        unique_buttons.end()) {
        return bindings;
    }
    bindings.gamepad_buttons = loaded_gamepad;
    return bindings;
}

#ifdef __ANDROID__
void save_legacy_bindings(const std::filesystem::path& directory,
                          const InputBindings& bindings) {
    if (directory.empty()) return;
    std::ofstream output(directory / "controls.txt", std::ios::trunc);
    for (const auto& keys : bindings.keys) {
        output << static_cast<long long>(keys[0]) << '\n';
    }
    for (const auto button : bindings.gamepad_buttons) {
        output << static_cast<int>(button) << '\n';
    }
}
#endif

std::size_t load_legacy_display_palette(
    const std::filesystem::path& directory) {
    if (directory.empty()) return 0;
    std::ifstream input(directory / "palette.txt");
    std::string id;
    if (!(input >> id)) return 0;
    const auto found = std::find_if(
        gameboy::display_palettes.begin(), gameboy::display_palettes.end(),
        [&id](const gameboy::DisplayPalette& palette) {
            return id == palette.id;
        });
    return found == gameboy::display_palettes.end()
               ? 0
               : static_cast<std::size_t>(found -
                                           gameboy::display_palettes.begin());
}

AppSettings load_portable_settings(
    const std::filesystem::path& preference_directory) {
    AppSettings settings;
    const auto path = portable_settings_path(preference_directory);
    const auto document = gbb::read_settings_file(path);
    if (!document.readable) {
        settings.bindings = load_legacy_bindings(preference_directory);
        settings.palette = load_legacy_display_palette(preference_directory);
        write_portable_settings(preference_directory, settings);
        return settings;
    }

    auto loaded_keys = settings.bindings.keys;
    auto loaded_buttons = settings.bindings.gamepad_buttons;
    auto loaded_shortcuts = settings.bindings.shortcuts;
    auto loaded_touch_positions = settings.touch.positions;
    std::array<float, 16> legacy_touch_positions{};
    std::array<bool, 8> has_legacy_touch_positions{};
    bool has_palette = false;
    bool has_video_mode = false;
    bool has_link_diagnostics = false;
    std::array<bool, 8> has_keyboard{};
    std::array<bool, 8> has_gamepad{};
    std::array<bool, shortcut_names.size()> has_shortcuts{};
    bool has_touch_scale = false;
    bool has_touch_opacity = false;
    bool has_touch_voxel_orbit = false;
    bool has_touch_menu_position = false;
    std::array<bool, touch_layout_count * touch_control_count>
        has_touch_positions{};
    constexpr std::array<const char*, 8> names{{
        "Right", "Left", "Up", "Down", "A", "B", "Select", "Start"}};
    for (const auto& entry : document.entries) {
        const auto& key = entry.key;
        const auto& value = entry.value;
        if (key == "palette") {
            has_palette = true;
            const auto found = std::find_if(
                gameboy::display_palettes.begin(),
                gameboy::display_palettes.end(),
                [&value](const gameboy::DisplayPalette& palette) {
                    return value == palette.id;
                });
            if (found != gameboy::display_palettes.end()) {
                settings.palette = static_cast<std::size_t>(
                    found - gameboy::display_palettes.begin());
            }
            continue;
        }
        if (key == "video.Mode") {
            has_video_mode = true;
            settings.video_mode = gameboy::video_mode_from_id(value);
            continue;
        }
        if (key == "link.Diagnostics") {
            has_link_diagnostics = true;
            settings.link_diagnostics = parse_bool_setting(
                value, settings.link_diagnostics);
            continue;
        }
        if (key == "touch.Size") {
            has_touch_scale = true;
            settings.touch.scale = parse_touch_value(
                value, settings.touch.scale, minimum_touch_scale,
                maximum_touch_scale);
            continue;
        }
        if (key == "touch.Opacity") {
            has_touch_opacity = true;
            settings.touch.opacity = parse_touch_value(
                value, settings.touch.opacity, minimum_touch_opacity,
                maximum_touch_opacity);
            continue;
        }
        if (key == "touch.VoxelOrbit") {
            has_touch_voxel_orbit = true;
            settings.touch.voxel_orbit = parse_bool_setting(
                value, settings.touch.voxel_orbit);
            continue;
        }
        if (key == "touch.MenuPosition") {
            has_touch_menu_position = true;
            settings.touch.menu_top_right = value == "top-right";
            continue;
        }
        bool touch_setting = false;
        for (std::size_t orientation = 0; orientation < touch_layout_count;
             ++orientation) {
            for (std::size_t control = 0; control < touch_control_count;
                 ++control) {
                const auto setting_name =
                    std::string("touch.") + touch_layout_names[orientation] +
                    '.' + touch_control_names[control];
                if (key != setting_name) continue;
                touch_setting = true;
                std::istringstream values(value);
                float x = 0.0F;
                float y = 0.0F;
                if (values >> x >> y) {
                    const auto index = orientation * touch_layout_stride +
                                       control * 2;
                    loaded_touch_positions[index] = std::clamp(
                        x, minimum_touch_position, maximum_touch_position);
                    loaded_touch_positions[index + 1] = std::clamp(
                        y, minimum_touch_position, maximum_touch_position);
                    has_touch_positions[orientation * touch_control_count +
                                        control] = true;
                }
            }
        }
        if (touch_setting) continue;
        for (std::size_t index = 0; index < button_names.size(); ++index) {
            if (key != std::string("touch.") + button_names[index]) continue;
            std::istringstream values(value);
            float x = 0.0F;
            float y = 0.0F;
            if (values >> x >> y) {
                legacy_touch_positions[index * 2] = std::clamp(
                    x, minimum_touch_position, maximum_touch_position);
                legacy_touch_positions[index * 2 + 1] = std::clamp(
                    y, minimum_touch_position, maximum_touch_position);
                has_legacy_touch_positions[index] = true;
            }
            break;
        }
        bool shortcut_setting = false;
        for (std::size_t index = 0; index < shortcut_names.size(); ++index) {
            if (key != std::string("keyboard.") + shortcut_names[index]) {
                continue;
            }
            has_shortcuts[index] = true;
            shortcut_setting = true;
            if (value == "None") {
                loaded_shortcuts[index] = SDLK_UNKNOWN;
            } else {
                const auto parsed = keyboard_key_from_setting(value);
                if (parsed != SDLK_UNKNOWN) loaded_shortcuts[index] = parsed;
            }
            break;
        }
        if (shortcut_setting) continue;
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (key == std::string("keyboard.") + names[index]) {
                has_keyboard[index] = true;
                std::array<SDL_Keycode, 2> parsed_keys{
                    SDLK_UNKNOWN, SDLK_UNKNOWN};
                bool parsed_mapping = value == "None";
                const auto whole = parsed_mapping
                                       ? SDLK_UNKNOWN
                                       : keyboard_key_from_setting(value);
                if (whole != SDLK_UNKNOWN) {
                    parsed_keys[0] = whole;
                    parsed_mapping = true;
                } else {
                    std::istringstream values(value);
                    std::string name;
                    std::size_t slot = 0;
                    while (slot < parsed_keys.size() && values >> name) {
                        if (name == "None") {
                            ++slot;
                            parsed_mapping = true;
                            continue;
                        }
                        const auto parsed = keyboard_key_from_setting(name);
                        if (parsed != SDLK_UNKNOWN) {
                            parsed_keys[slot++] = parsed;
                            parsed_mapping = true;
                        }
                    }
                }
                if (parsed_mapping) loaded_keys[index] = parsed_keys;
            } else if (key == std::string("gamepad.") + names[index]) {
                has_gamepad[index] = true;
                const auto parsed = gamepad_button_from_setting(value);
                if (parsed >= 0 && parsed < SDL_GAMEPAD_BUTTON_COUNT) {
                    loaded_buttons[index] = parsed;
                }
            }
        }
    }
    std::vector<SDL_Keycode> unique_keys;
    for (const auto& keys : loaded_keys) {
        for (const auto key : keys) {
            if (key != SDLK_UNKNOWN) unique_keys.push_back(key);
        }
    }
    for (const auto key : loaded_shortcuts) {
        if (key != SDLK_UNKNOWN) unique_keys.push_back(key);
    }
    std::sort(unique_keys.begin(), unique_keys.end());
    if (std::adjacent_find(unique_keys.begin(), unique_keys.end()) ==
        unique_keys.end()) {
        settings.bindings.keys = loaded_keys;
    }
    if (std::adjacent_find(unique_keys.begin(), unique_keys.end()) ==
        unique_keys.end()) {
        settings.bindings.shortcuts = loaded_shortcuts;
    }
    auto unique_buttons = loaded_buttons;
    std::sort(unique_buttons.begin(), unique_buttons.end());
    if (std::adjacent_find(unique_buttons.begin(), unique_buttons.end()) ==
        unique_buttons.end()) {
        settings.bindings.gamepad_buttons = loaded_buttons;
    }
    const auto legacy_position = [&legacy_touch_positions](
                                     const std::size_t index) {
        return std::pair<float, float>{legacy_touch_positions[index * 2],
                                       legacy_touch_positions[index * 2 + 1]};
    };
    const auto legacy_dpad = [&]() {
        float x = 0.0F;
        float y = 0.0F;
        std::size_t count = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            if (!has_legacy_touch_positions[index]) continue;
            x += legacy_touch_positions[index * 2];
            y += legacy_touch_positions[index * 2 + 1];
            ++count;
        }
        return count == 0 ? std::pair<float, float>{0.27F, 0.82F}
                          : std::pair<float, float>{x / count, y / count};
    };
    for (std::size_t orientation = 0; orientation < touch_layout_count;
         ++orientation) {
        const auto dpad = legacy_dpad();
        const auto legacy_controls = std::array<std::pair<float, float>, 5>{
            dpad, legacy_position(4), legacy_position(5),
            legacy_position(6), legacy_position(7)};
        for (std::size_t control = 0; control < touch_control_count;
             ++control) {
            const auto entry = orientation * touch_control_count + control;
            if (has_touch_positions[entry]) continue;
            const auto legacy_index = control == 0 ? 0 : control + 3;
            if (!has_legacy_touch_positions[legacy_index] &&
                (control != 0 ||
                 !std::any_of(has_legacy_touch_positions.begin(),
                              has_legacy_touch_positions.begin() + 4,
                              [](const bool value) { return value; }))) {
                continue;
            }
            const auto index = orientation * touch_layout_stride + control * 2;
            loaded_touch_positions[index] = legacy_controls[control].first;
            loaded_touch_positions[index + 1] = legacy_controls[control].second;
        }
    }
    settings.touch.positions = loaded_touch_positions;
    append_missing_portable_settings(path, settings, has_palette,
                                     has_keyboard, has_gamepad, has_shortcuts,
                                     has_video_mode, has_link_diagnostics,
                                     has_touch_scale, has_touch_opacity,
                                     has_touch_voxel_orbit,
                                     has_touch_menu_position,
                                     has_touch_positions);
    return settings;
}

AppSettings load_app_settings(
    const std::filesystem::path& preference_directory) {
    return load_portable_settings(preference_directory);
}

void save_app_settings(const std::filesystem::path& preference_directory,
                       const InputBindings& bindings,
                       const std::size_t palette) {
    if (palette >= gameboy::display_palettes.size()) return;
    auto settings = load_app_settings(preference_directory);
    settings.bindings = bindings;
    settings.palette = palette;
    write_portable_settings(preference_directory, settings);
}

gameboy::VideoMode load_video_mode(const std::filesystem::path& directory) {
    return load_app_settings(directory).video_mode;
}

bool load_link_diagnostics(const std::filesystem::path& directory) {
    if (load_app_settings(directory).link_diagnostics) return true;
    std::error_code error;
    const auto working_directory = std::filesystem::current_path(error);
    if (error || working_directory.empty() || working_directory == directory) {
        return false;
    }
    const auto document = gbb::read_settings_file(
        working_directory / "settings.ini");
    if (!document.readable) return false;
    for (const auto& entry : document.entries) {
        if (entry.key != "link.Diagnostics") continue;
        return parse_bool_setting(entry.value, false);
    }
    return false;
}

void save_video_mode(const std::filesystem::path& directory,
                     const gameboy::VideoMode mode) {
    auto settings = load_app_settings(directory);
    settings.video_mode = mode;
    write_portable_settings(directory, settings);
}

TouchControlSettings load_touch_control_settings(
    const std::filesystem::path& directory) {
    return load_app_settings(directory).touch;
}

void save_touch_control_settings(const std::filesystem::path& directory,
                                 const float scale, const float opacity) {
    auto settings = load_app_settings(directory);
    settings.touch.scale = std::clamp(scale, minimum_touch_scale,
                                      maximum_touch_scale);
    settings.touch.opacity = std::clamp(opacity, minimum_touch_opacity,
                                        maximum_touch_opacity);
    write_portable_settings(directory, settings);
}

void save_touch_voxel_orbit(const std::filesystem::path& directory,
                            const bool enabled) {
    auto settings = load_app_settings(directory);
    settings.touch.voxel_orbit = enabled;
    write_portable_settings(directory, settings);
}

void save_touch_menu_position(const std::filesystem::path& directory,
                              const bool top_right) {
    auto settings = load_app_settings(directory);
    settings.touch.menu_top_right = top_right;
    write_portable_settings(directory, settings);
}

std::array<float, touch_layout_count * touch_layout_stride>
load_touch_control_layout(const std::filesystem::path& directory) {
    return load_app_settings(directory).touch.positions;
}

void save_touch_control_layout(
    const std::filesystem::path& directory,
    const std::array<float, touch_layout_count * touch_layout_stride>&
        positions) {
    auto settings = load_app_settings(directory);
    for (std::size_t index = 0; index < positions.size(); ++index) {
        settings.touch.positions[index] = std::clamp(
            positions[index], minimum_touch_position, maximum_touch_position);
    }
    write_portable_settings(directory, settings);
}

InputBindings load_bindings(const std::filesystem::path& directory) {
    return load_app_settings(directory).bindings;
}

std::size_t load_display_palette(const std::filesystem::path& directory) {
    return load_app_settings(directory).palette;
}

void save_bindings(const std::filesystem::path& directory,
                   const InputBindings& bindings) {
    save_app_settings(directory, bindings, load_display_palette(directory));
}

void save_display_palette(const std::filesystem::path& directory,
                          const std::size_t palette) {
    save_app_settings(directory, load_bindings(directory), palette);
}
