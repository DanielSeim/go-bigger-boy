#include "gameboy/emulator.hpp"
#include "gameboy/display_palette.hpp"
#include "gameboy/rom_library.hpp"
#ifndef __ANDROID__
#include "update_checker.hpp"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
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
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "windows_dashboard.hpp"
#endif

#ifdef __ANDROID__
#include <jni.h>
#endif

namespace {

#ifndef GBB_VERSION
#define GBB_VERSION "0.13.4"
#endif

#ifdef __ANDROID__
struct AndroidRomRequest {
    std::string path;
    std::string display_name;
};

std::mutex android_rom_request_mutex;
std::optional<AndroidRomRequest> android_rom_request;

std::optional<AndroidRomRequest> take_android_rom_request() {
    std::lock_guard<std::mutex> lock(android_rom_request_mutex);
    auto request = std::move(android_rom_request);
    android_rom_request.reset();
    return request;
}
#endif

[[noreturn]] void sdl_error(const std::string& action) {
    throw std::runtime_error(action + ": " + SDL_GetError());
}

class SdlResources {
public:
    SdlResources() {
#ifdef __ANDROID__
        if (!SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1")) {
            sdl_error("Could not trap the Android back button");
        }
#endif
        if (!SDL_SetAppMetadata("Go Bigger Boy (GBB)", GBB_VERSION,
                                "go-bigger-boy")) {
            sdl_error("Could not set application metadata");
        }
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            sdl_error("Could not initialize SDL");
        }
        window = SDL_CreateWindow(
            "Go Bigger Boy (GBB) - Drop a ROM here or press Ctrl+O", 640, 576,
            SDL_WINDOW_RESIZABLE);
        if (window == nullptr) sdl_error("Could not create window");
        renderer = SDL_CreateRenderer(window, nullptr);
        if (renderer == nullptr) sdl_error("Could not create renderer");
        if (!SDL_SetRenderLogicalPresentation(
                renderer, static_cast<int>(gameboy::Ppu::screen_width),
                static_cast<int>(gameboy::Ppu::screen_height),
                SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
            sdl_error("Could not configure logical presentation");
        }
        texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(gameboy::Ppu::screen_width),
            static_cast<int>(gameboy::Ppu::screen_height));
        if (texture == nullptr) sdl_error("Could not create framebuffer texture");
        if (!SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST)) {
            sdl_error("Could not configure nearest-neighbor scaling");
        }
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            const SDL_AudioSpec audio_spec{
                SDL_AUDIO_S16, 2, static_cast<int>(gameboy::Apu::sample_rate)};
            audio_stream = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, nullptr, nullptr);
            if (audio_stream != nullptr &&
                !SDL_ResumeAudioStreamDevice(audio_stream)) {
                SDL_DestroyAudioStream(audio_stream);
                audio_stream = nullptr;
            }
        }
        if (audio_stream == nullptr) {
            std::cerr << "Warning: audio output is unavailable: "
                      << SDL_GetError() << '\n';
        }
        static_cast<void>(SDL_SetRenderDrawColor(renderer, 16, 20, 16, 255));
    }

    ~SdlResources() {
        if (camera != nullptr) SDL_CloseCamera(camera);
        if (camera_frame != nullptr) SDL_DestroySurface(camera_frame);
        if (gamepad != nullptr) {
            static_cast<void>(SDL_RumbleGamepad(gamepad, 0, 0, 0));
            SDL_CloseGamepad(gamepad);
        }
        if (audio_stream != nullptr) SDL_DestroyAudioStream(audio_stream);
        if (texture != nullptr) SDL_DestroyTexture(texture);
        if (renderer != nullptr) SDL_DestroyRenderer(renderer);
        if (window != nullptr) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    SdlResources(const SdlResources&) = delete;
    SdlResources& operator=(const SdlResources&) = delete;

    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    SDL_Gamepad* gamepad{};
    SDL_AudioStream* audio_stream{};
    SDL_Camera* camera{};
    SDL_Surface* camera_frame{};
    bool mirror_camera{};
    bool camera_back_facing{};
    bool camera_warning_shown{};
    std::chrono::steady_clock::time_point next_camera_frame{};
    bool rumble_output_active{};
    bool rumble_warning_shown{};
    std::chrono::steady_clock::time_point rumble_refresh{};
#ifdef __ANDROID__
    struct TouchPoint {
        SDL_FingerID id{};
        float x{};
        float y{};
    };
    std::vector<TouchPoint> touches;
    std::array<bool, 8> touch_buttons{};
#endif
};

struct DialogState {
    std::mutex mutex;
    bool active{};
    std::optional<std::string> selected_path;
    std::optional<std::string> error;
};

constexpr std::array<gameboy::Button, 8> button_order{
    gameboy::Button::right, gameboy::Button::left, gameboy::Button::up,
    gameboy::Button::down, gameboy::Button::a, gameboy::Button::b,
    gameboy::Button::select, gameboy::Button::start,
};

constexpr std::array<const char*, 8> button_names{
    "Right", "Left", "Up", "Down", "A", "B", "Select", "Start",
};

struct InputBindings {
    std::array<std::array<SDL_Keycode, 2>, 8> keys{{
        {{SDLK_RIGHT, SDLK_UNKNOWN}}, {{SDLK_LEFT, SDLK_UNKNOWN}},
        {{SDLK_UP, SDLK_UNKNOWN}}, {{SDLK_DOWN, SDLK_UNKNOWN}},
        {{SDLK_X, SDLK_UNKNOWN}}, {{SDLK_Z, SDLK_UNKNOWN}},
        {{SDLK_BACKSPACE, SDLK_UNKNOWN}}, {{SDLK_RETURN, SDLK_UNKNOWN}},
    }};
    std::array<SDL_GamepadButton, 8> gamepad_buttons{
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH,
        SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
    };
};

enum class BindingDevice { keyboard, gamepad };

struct BindingConfiguration {
    BindingDevice device{};
    std::size_t index{};
    std::size_t slot{};
};

enum class DashboardAction { resume, open_rom, palette, recent_rom, quit };

struct DashboardItem {
    DashboardAction action{};
    std::size_t recent_index{};
    std::string label;
};

constexpr std::size_t dashboard_visible_rows = 5;
constexpr float dashboard_first_row_y = 39.0F;
constexpr float dashboard_row_height = 18.0F;

constexpr std::uintmax_t maximum_quick_state_size = 2 * 1024 * 1024;

std::filesystem::path preference_directory() {
    char* raw_path = SDL_GetPrefPath("Go Bigger Boy", "GBB");
    if (raw_path == nullptr) return {};
    const auto path = std::filesystem::u8path(raw_path);
    SDL_free(raw_path);
    return path;
}

struct WindowGeometry {
    int x{};
    int y{};
    int width{};
    int height{};
};

bool geometry_is_visible(const WindowGeometry& geometry) {
    int display_count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
    if (displays == nullptr) return true;
    const SDL_Rect window{geometry.x, geometry.y, geometry.width,
                          geometry.height};
    bool visible = false;
    for (int index = 0; index < display_count; ++index) {
        SDL_Rect bounds{};
        if (!SDL_GetDisplayBounds(displays[index], &bounds)) continue;
        SDL_Rect intersection{};
        if (SDL_GetRectIntersection(&window, &bounds, &intersection) &&
            intersection.w >= 64 && intersection.h >= 64) {
            visible = true;
            break;
        }
    }
    SDL_free(displays);
    return visible;
}

void restore_game_window_geometry(
    SDL_Window* window, const std::filesystem::path& directory) {
    if (directory.empty()) return;
    std::ifstream input(directory / "game-window.txt");
    WindowGeometry geometry;
    if (!(input >> geometry.x >> geometry.y >> geometry.width >>
          geometry.height) ||
        geometry.width < 320 || geometry.height < 288 ||
        !geometry_is_visible(geometry)) {
        return;
    }
    static_cast<void>(
        SDL_SetWindowSize(window, geometry.width, geometry.height));
    static_cast<void>(SDL_SetWindowPosition(window, geometry.x, geometry.y));
}

void save_game_window_geometry(
    SDL_Window* window, const std::filesystem::path& directory) {
    if (directory.empty() ||
        (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0) {
        return;
    }
    WindowGeometry geometry;
    if (!SDL_GetWindowPosition(window, &geometry.x, &geometry.y) ||
        !SDL_GetWindowSize(window, &geometry.width, &geometry.height) ||
        geometry.width < 320 || geometry.height < 288) {
        return;
    }
    std::ofstream output(directory / "game-window.txt", std::ios::trunc);
    output << geometry.x << ' ' << geometry.y << ' ' << geometry.width << ' '
           << geometry.height << '\n';
}

std::vector<std::string> load_legacy_recent_roms(
    const std::filesystem::path& directory) {
    std::vector<std::string> paths;
    if (directory.empty()) return paths;
    std::ifstream input(directory / "recent-roms.txt");
    std::string path;
    while (paths.size() < 9 && std::getline(input, path)) {
        if (!path.empty()) paths.push_back(path);
    }
    return paths;
}

std::vector<std::string> recent_paths(const gameboy::RomLibrary& library) {
    std::vector<std::string> paths;
    paths.reserve(library.entries().size());
    for (const auto& entry : library.entries()) {
        paths.push_back(entry.path.u8string());
    }
    return paths;
}

gameboy::RomLibrary load_rom_library(
    const std::filesystem::path& directory) {
    auto library = gameboy::RomLibrary::load(directory);
    if (!library.entries().empty()) return library;

    // Migrate the path-only dashboard history from older GBB versions. Invalid
    // or missing files are harmless and simply disappear from the new library.
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    for (const auto& path : load_legacy_recent_roms(directory)) {
        try {
            library.remember(path, gameboy::inspect_rom_file(path), timestamp--);
        } catch (const std::exception&) {
        }
    }
    if (!library.entries().empty()) library.save(directory);
    return library;
}

std::string dashboard_text(std::string text, const std::size_t maximum = 16) {
    for (auto& character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32 || byte > 126) character = '?';
    }
    if (text.size() > maximum) {
        text.resize(maximum > 3 ? maximum - 3 : maximum);
        if (maximum > 3) text += "...";
    }
    return text;
}

std::string rom_filename(const std::string& path) {
    auto name = std::filesystem::u8path(path).filename().u8string();
#ifdef __ANDROID__
    if (name.size() > 17 && name[16] == '-' &&
        std::all_of(name.begin(), name.begin() + 16, [](const char character) {
            return std::isxdigit(static_cast<unsigned char>(character)) != 0;
        })) {
        name.erase(0, 17);
    }
#endif
    return name.empty() ? path : name;
}

std::string rom_display_name(const std::string& path) {
    return dashboard_text(rom_filename(path));
}

std::vector<DashboardItem> dashboard_items(
    const bool can_resume, const std::vector<std::string>& recent) {
    std::vector<DashboardItem> items;
    items.reserve(recent.size() + 4);
    if (can_resume) {
        items.push_back({DashboardAction::resume, 0, "Resume game"});
    }
    items.push_back({DashboardAction::open_rom, 0, "+ Open a ROM"});
    items.push_back({DashboardAction::palette, 0, "Display palette"});
    for (std::size_t index = 0; index < recent.size(); ++index) {
        items.push_back({DashboardAction::recent_rom, index,
                         rom_display_name(recent[index])});
    }
    items.push_back({DashboardAction::quit, 0, "Exit GBB"});
    return items;
}

std::size_t dashboard_first_visible(const std::size_t selection,
                                    const std::size_t item_count) {
    if (item_count <= dashboard_visible_rows ||
        selection < dashboard_visible_rows) {
        return 0;
    }
    return std::min(selection - dashboard_visible_rows + 1,
                    item_count - dashboard_visible_rows);
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

#ifdef __ANDROID__
void save_legacy_display_palette(const std::filesystem::path& directory,
                                 const std::size_t palette) {
    if (directory.empty() || palette >= gameboy::display_palettes.size()) return;
    std::ofstream output(directory / "palette.txt", std::ios::trunc);
    output << gameboy::display_palettes[palette].id << '\n';
}
#endif

struct AppSettings {
    InputBindings bindings;
    std::size_t palette{};
};

#ifndef __ANDROID__
std::filesystem::path portable_settings_path() {
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
}

std::string trimmed_setting(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value.erase(0, first);
    const auto last = value.find_last_not_of(" \t\r\n");
    value.resize(last + 1);
    return value;
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
    const std::array<bool, 8>& has_gamepad) {
    const auto complete = has_palette &&
        std::all_of(has_keyboard.begin(), has_keyboard.end(),
                    [](const bool value) { return value; }) &&
        std::all_of(has_gamepad.begin(), has_gamepad.end(),
                    [](const bool value) { return value; });
    if (complete) return;
    std::ofstream output(path, std::ios::app);
    if (!output) {
        std::cerr << "Warning: could not complete portable settings file: "
                  << path << '\n';
        return;
    }
    output << "\n# Missing entries added automatically by GBB\n";
    if (!has_palette) {
        output << "palette = "
               << gameboy::display_palettes[settings.palette].id << '\n';
    }
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        if (has_keyboard[index]) continue;
        output << "keyboard." << button_names[index] << " = "
               << SDL_GetKeyName(settings.bindings.keys[index][0]);
        if (settings.bindings.keys[index][1] != SDLK_UNKNOWN) {
            output << ' '
                   << SDL_GetKeyName(settings.bindings.keys[index][1]);
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
}

void write_portable_settings(const AppSettings& settings) {
    const auto path = portable_settings_path();
    if (path.empty()) return;
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        std::cerr << "Warning: could not write portable settings file: "
                  << path << '\n';
        return;
    }
    output << "# Go Bigger Boy portable settings\n"
              "# Copy this file beside another GBB installation to share "
              "these settings.\n"
              "# Add an optional second keyboard key after the first, for "
              "example: Z Y\n\n"
              "palette = "
           << gameboy::display_palettes[settings.palette].id << "\n\n";
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        output << "keyboard." << button_names[index] << " = "
               << SDL_GetKeyName(settings.bindings.keys[index][0]);
        if (settings.bindings.keys[index][1] != SDLK_UNKNOWN) {
            output << ' '
                   << SDL_GetKeyName(settings.bindings.keys[index][1]);
        }
        output << '\n';
    }
    output << '\n';
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        output << "gamepad." << button_names[index] << " = "
               << gamepad_button_setting_name(
                      settings.bindings.gamepad_buttons[index])
               << '\n';
    }
}

AppSettings load_portable_settings(
    const std::filesystem::path& preference_directory) {
    AppSettings settings;
    const auto path = portable_settings_path();
    std::ifstream input(path);
    if (!input) {
        settings.bindings = load_legacy_bindings(preference_directory);
        settings.palette = load_legacy_display_palette(preference_directory);
        write_portable_settings(settings);
        return settings;
    }

    auto loaded_keys = settings.bindings.keys;
    auto loaded_buttons = settings.bindings.gamepad_buttons;
    bool has_palette = false;
    std::array<bool, 8> has_keyboard{};
    std::array<bool, 8> has_gamepad{};
    constexpr std::array<const char*, 8> names{{
        "Right", "Left", "Up", "Down", "A", "B", "Select", "Start"}};
    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find_first_of("#;");
        if (comment != std::string::npos) line.resize(comment);
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = trimmed_setting(line.substr(0, separator));
        const auto value = trimmed_setting(line.substr(separator + 1));
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
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (key == std::string("keyboard.") + names[index]) {
                has_keyboard[index] = true;
                std::array<SDL_Keycode, 2> parsed_keys{
                    SDLK_UNKNOWN, SDLK_UNKNOWN};
                const auto whole = SDL_GetKeyFromName(value.c_str());
                if (whole != SDLK_UNKNOWN) {
                    parsed_keys[0] = whole;
                } else {
                    std::istringstream values(value);
                    std::string name;
                    std::size_t slot = 0;
                    while (slot < parsed_keys.size() && values >> name) {
                        const auto parsed = SDL_GetKeyFromName(name.c_str());
                        if (parsed != SDLK_UNKNOWN) parsed_keys[slot++] = parsed;
                    }
                }
                if (parsed_keys[0] != SDLK_UNKNOWN) {
                    loaded_keys[index] = parsed_keys;
                }
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
    std::sort(unique_keys.begin(), unique_keys.end());
    if (std::adjacent_find(unique_keys.begin(), unique_keys.end()) ==
        unique_keys.end()) {
        settings.bindings.keys = loaded_keys;
    }
    auto unique_buttons = loaded_buttons;
    std::sort(unique_buttons.begin(), unique_buttons.end());
    if (std::adjacent_find(unique_buttons.begin(), unique_buttons.end()) ==
        unique_buttons.end()) {
        settings.bindings.gamepad_buttons = loaded_buttons;
    }
    append_missing_portable_settings(path, settings, has_palette,
                                     has_keyboard, has_gamepad);
    return settings;
}
#endif

AppSettings load_app_settings(
    const std::filesystem::path& preference_directory) {
#ifdef __ANDROID__
    return {load_legacy_bindings(preference_directory),
            load_legacy_display_palette(preference_directory)};
#else
    return load_portable_settings(preference_directory);
#endif
}

void save_app_settings(const std::filesystem::path& preference_directory,
                       const InputBindings& bindings,
                       const std::size_t palette) {
    if (palette >= gameboy::display_palettes.size()) return;
#ifdef __ANDROID__
    save_legacy_bindings(preference_directory, bindings);
    save_legacy_display_palette(preference_directory, palette);
#else
    static_cast<void>(preference_directory);
    write_portable_settings({bindings, palette});
#endif
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

std::filesystem::path quick_state_path(
    const std::filesystem::path& preference_path,
    const gameboy::Emulator& emulator) {
    if (preference_path.empty()) {
        throw std::runtime_error("Could not locate the preferences directory");
    }
    std::ostringstream name;
    name << std::hex << std::setw(16) << std::setfill('0')
         << emulator.rom_fingerprint() << ".gbbs";
    return preference_path / "states" / name.str();
}

void replace_file_atomically(const std::filesystem::path& temporary,
                             const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "Could not publish quick save");
    }
#else
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "Could not publish quick save");
    }
#endif
}

void save_printer_bitmap(const std::filesystem::path& path,
                         const gameboy::PrinterImage& image) {
    const auto bytes = gameboy::encode_printer_bmp(image);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Could not create printer image: " +
                                 path.u8string());
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("Could not write printer image: " +
                                 path.u8string());
    }
}

void save_completed_prints(gameboy::Emulator* emulator, SDL_Window* window,
                           const std::filesystem::path& preference_path,
                           const std::string& current_rom,
                           std::uint64_t& print_sequence) {
    if (emulator == nullptr) return;
    auto images = emulator->bus().take_printer_images();
    if (images.empty()) return;

    const auto directory = preference_path.empty()
                               ? std::filesystem::current_path() / "GBB Prints"
                               : preference_path / "prints";
    std::filesystem::create_directories(directory);
    auto rom_name = std::filesystem::u8path(current_rom).stem().u8string();
    if (rom_name.empty()) rom_name = "gameboy";
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now()
                                   .time_since_epoch())
                               .count();
    std::vector<std::filesystem::path> paths;
    paths.reserve(images.size());
    for (const auto& image : images) {
        const auto filename = rom_name + "-print-" +
                              std::to_string(timestamp) + "-" +
                              std::to_string(++print_sequence) + ".bmp";
        auto path = directory / std::filesystem::u8path(filename);
        save_printer_bitmap(path, image);
        paths.push_back(std::move(path));
    }

    std::ostringstream message;
    message << "Saved " << paths.size() << " printer image";
    if (paths.size() != 1) message << 's';
    message << " to:\n" << directory.u8string();
    const auto text = message.str();
    std::cerr << text << '\n';
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_INFORMATION, "Game Boy Printer", text.c_str(), window));
}

void save_quick_state(const std::filesystem::path& preference_path,
                      const gameboy::Emulator& emulator) {
    const auto path = quick_state_path(preference_path, emulator);
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    try {
        const auto state = emulator.save_state();
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Could not open temporary quick-save file");
        }
        output.write(reinterpret_cast<const char*>(state.data()),
                     static_cast<std::streamsize>(state.size()));
        output.flush();
        if (!output) throw std::runtime_error("Could not write quick save");
        output.close();
        replace_file_atomically(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void load_quick_state(const std::filesystem::path& preference_path,
                      gameboy::Emulator& emulator) {
    const auto path = quick_state_path(preference_path, emulator);
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (!size_error && size > maximum_quick_state_size) {
        throw std::runtime_error("Quick save is too large");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("No quick save exists for this ROM");
    std::vector<std::uint8_t> state(
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    if (input.bad()) throw std::runtime_error("Could not read quick save");
    emulator.load_state(state);
}

void SDLCALL file_dialog_callback(void* userdata,
                                  const char* const* filelist, int) {
    auto& state = *static_cast<DialogState*>(userdata);
    std::lock_guard<std::mutex> lock(state.mutex);
    state.active = false;
    if (filelist == nullptr) {
        state.error = SDL_GetError();
    } else if (filelist[0] != nullptr) {
        state.selected_path = filelist[0];
    }
}

void show_rom_dialog(DialogState& state, SDL_Window* window) {
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.active) return;
        state.active = true;
    }
    static constexpr SDL_DialogFileFilter filters[] = {
        {"Game Boy ROMs", "gb;gbc"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(file_dialog_callback, &state, window, filters,
                           static_cast<int>(std::size(filters)), nullptr, false);
}

bool dialog_active(DialogState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.active;
}

void collect_dialog_result(DialogState& state,
                           std::optional<std::string>& path,
                           std::optional<std::string>& error) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.selected_path) {
        path = std::move(*state.selected_path);
        state.selected_path.reset();
    }
    if (state.error) {
        error = std::move(*state.error);
        state.error.reset();
    }
}

std::optional<gameboy::Button> keyboard_button(const InputBindings& bindings,
                                               const SDL_Keycode key) {
    for (std::size_t index = 0; index < bindings.keys.size(); ++index) {
        if (std::find(bindings.keys[index].begin(), bindings.keys[index].end(),
                      key) != bindings.keys[index].end()) {
            return button_order[index];
        }
    }
    return std::nullopt;
}

std::optional<gameboy::Button> gamepad_button(const InputBindings& bindings,
                                              const Uint8 button) {
    for (std::size_t index = 0; index < bindings.gamepad_buttons.size(); ++index) {
        if (bindings.gamepad_buttons[index] ==
            static_cast<SDL_GamepadButton>(button)) {
            return button_order[index];
        }
    }
    return std::nullopt;
}

#ifdef __ANDROID__
std::optional<std::size_t> touch_button_index(const float x, const float y) {
    const auto inside = [x, y](const float center_x, const float center_y,
                               const float radius) {
        const auto dx = x - center_x;
        const auto dy = y - center_y;
        return dx * dx + dy * dy <= radius * radius;
    };
    if (inside(0.80F, 0.70F, 0.11F)) return 4; // A
    if (inside(0.66F, 0.80F, 0.11F)) return 5; // B
    if (inside(0.56F, 0.91F, 0.075F)) return 7; // Start
    if (inside(0.43F, 0.91F, 0.075F)) return 6; // Select
    if (x < 0.42F && y > 0.48F) {
        const auto dx = x - 0.20F;
        const auto dy = y - 0.73F;
        if (dx * dx + dy * dy < 0.0025F ||
            dx * dx + dy * dy > 0.050F) {
            return std::nullopt;
        }
        if (std::abs(dx) > std::abs(dy)) return dx > 0 ? 0 : 1;
        return dy < 0 ? 2 : 3;
    }
    return std::nullopt;
}

void refresh_touch_buttons(gameboy::Emulator* emulator, SdlResources& sdl) {
    std::array<bool, 8> pressed{};
    for (const auto& touch : sdl.touches) {
        if (const auto index = touch_button_index(touch.x, touch.y)) {
            pressed[*index] = true;
        }
    }
    if (emulator != nullptr) {
        for (std::size_t index = 0; index < pressed.size(); ++index) {
            if (pressed[index] != sdl.touch_buttons[index]) {
                emulator->set_button(button_order[index], pressed[index]);
            }
        }
    }
    sdl.touch_buttons = pressed;
}

void clear_touch_buttons(gameboy::Emulator* emulator, SdlResources& sdl) {
    sdl.touches.clear();
    if (emulator != nullptr) {
        for (std::size_t index = 0; index < sdl.touch_buttons.size(); ++index) {
            if (sdl.touch_buttons[index]) {
                emulator->set_button(button_order[index], false);
            }
        }
    }
    sdl.touch_buttons.fill(false);
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
    return {x / static_cast<float>(gameboy::Ppu::screen_width),
            y / static_cast<float>(gameboy::Ppu::screen_height)};
}
#endif

bool reserved_gameplay_key(const SDL_Keycode key) {
    switch (key) {
    case SDLK_ESCAPE:
    case SDLK_SPACE:
    case SDLK_F1:
    case SDLK_F5:
    case SDLK_F8:
    case SDLK_F11:
        return true;
    default:
        return false;
    }
}

void release_all_buttons(gameboy::Emulator& emulator) {
    for (const auto button : button_order) emulator.set_button(button, false);
}

void stop_rumble(SdlResources& sdl) noexcept {
    if (sdl.gamepad != nullptr && sdl.rumble_output_active) {
        static_cast<void>(SDL_RumbleGamepad(sdl.gamepad, 0, 0, 0));
    }
    sdl.rumble_output_active = false;
    sdl.rumble_refresh = {};
}

void update_rumble(const gameboy::Emulator* emulator, SdlResources& sdl,
                   const bool enabled) {
    const auto desired = enabled && emulator != nullptr &&
                         emulator->has_rumble() && emulator->rumble_active();
    if (!desired || sdl.gamepad == nullptr) {
        stop_rumble(sdl);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < sdl.rumble_refresh) return;
    constexpr auto duration_ms = 500U;
    if (SDL_RumbleGamepad(sdl.gamepad, 0xC000, 0x6000, duration_ms)) {
        sdl.rumble_output_active = true;
        sdl.rumble_refresh = now + std::chrono::milliseconds(250);
    } else {
        sdl.rumble_output_active = false;
        sdl.rumble_refresh = now + std::chrono::seconds(5);
        if (!sdl.rumble_warning_shown) {
            std::cerr << "Warning: the connected gamepad does not provide "
                         "rumble output: "
                      << SDL_GetError() << '\n';
            sdl.rumble_warning_shown = true;
        }
    }
}

void update_window_title(SDL_Window* window, const std::string& current_rom,
                         const bool paused,
                         const std::optional<BindingConfiguration>& configuring) {
    std::string title = "Go Bigger Boy (GBB)";
    if (configuring) {
        title += configuring->device == BindingDevice::keyboard
                     ? " - Press a key for "
                     : " - Press a gamepad button for ";
        title += button_names[configuring->index];
        if (configuring->device == BindingDevice::keyboard) {
            title += configuring->slot == 0
                         ? " (primary)"
                         : " (secondary; Space: none)";
        }
        title += " (Esc: cancel)";
    } else {
        if (!current_rom.empty()) {
            title += " - " + rom_filename(current_rom);
        } else {
            title += " - Drop a ROM here or press Ctrl+O";
        }
        if (paused) title += " [PAUSED]";
        title += "  (F1: Help)";
    }
    if (!SDL_SetWindowTitle(window, title.c_str())) {
        sdl_error("Could not update window title");
    }
}

enum class ControlsAction { cancel, keyboard, gamepad, reset };

void begin_binding_configuration(
    InputBindings& bindings, InputBindings& backup,
    std::optional<BindingConfiguration>& configuring,
    const BindingDevice device) {
    backup = bindings;
    if (device == BindingDevice::keyboard) {
        for (auto& keys : bindings.keys) keys.fill(SDLK_UNKNOWN);
    }
    configuring = BindingConfiguration{device, 0, 0};
}

ControlsAction show_controls_dialog(SDL_Window* window,
                                    const InputBindings& bindings) {
    std::ostringstream message;
    message << "Current controls:\n";
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        message << button_names[index] << ": "
                << SDL_GetKeyName(bindings.keys[index][0]);
        if (bindings.keys[index][1] != SDLK_UNKNOWN) {
            message << " or " << SDL_GetKeyName(bindings.keys[index][1]);
        }
        message << " / "
                << SDL_GetGamepadStringForButton(
                       bindings.gamepad_buttons[index])
                << '\n';
    }
    message << "\nChoose which controls to configure. Keyboard setup asks "
               "for a primary and optional secondary key; press Space to "
               "skip a secondary key.";

    constexpr std::array<SDL_MessageBoxButtonData, 4> buttons{{
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
        {0, 1, "Keyboard"},
        {0, 2, "Gamepad"},
        {0, 3, "Restore defaults"},
    }};
    const auto text = message.str();
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_INFORMATION, window, "Configure controls", text.c_str(),
        static_cast<int>(buttons.size()), buttons.data(), nullptr,
    };
    auto selection = 0;
    if (!SDL_ShowMessageBox(&box, &selection)) return ControlsAction::cancel;
    switch (selection) {
    case 1: return ControlsAction::keyboard;
    case 2: return ControlsAction::gamepad;
    case 3: return ControlsAction::reset;
    default: return ControlsAction::cancel;
    }
}

std::optional<std::size_t> show_palette_dialog(SDL_Window* window,
                                               const std::size_t current) {
    std::array<SDL_MessageBoxButtonData,
               gameboy::display_palettes.size() + 1>
        buttons{};
    for (std::size_t index = 0; index < gameboy::display_palettes.size();
         ++index) {
        buttons[index] = {0, static_cast<int>(index),
                          gameboy::display_palettes[index].name};
    }
    buttons.back() = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, -1, "Cancel"};

    const auto current_name = current < gameboy::display_palettes.size()
                                  ? gameboy::display_palettes[current].name
                                  : gameboy::display_palettes.front().name;
    const auto message = std::string("Current palette: ") + current_name +
                         "\n\nChoose a display palette:";
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_INFORMATION, window, "Display palette", message.c_str(),
        static_cast<int>(buttons.size()), buttons.data(), nullptr,
    };
    auto selection = -1;
    if (!SDL_ShowMessageBox(&box, &selection) || selection < 0 ||
        static_cast<std::size_t>(selection) >=
            gameboy::display_palettes.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(selection);
}

void choose_display_palette(gameboy::Emulator* emulator, SdlResources& sdl,
                            const std::filesystem::path& preference_path,
                            std::size_t& display_palette) {
    if (emulator != nullptr) release_all_buttons(*emulator);
    const auto selected = show_palette_dialog(sdl.window, display_palette);
    if (!selected) return;
    display_palette = *selected;
    if (emulator != nullptr) {
        emulator->set_dmg_compatibility_colors(
            gameboy::display_palettes[display_palette].cgb_compatibility);
    }
    save_display_palette(preference_path, display_palette);
}

bool confirm_exit(SDL_Window* window) {
    constexpr std::array<SDL_MessageBoxButtonData, 2> buttons{{
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Exit"},
    }};
    constexpr auto message =
        "Are you sure you want to close Go Bigger Boy?";
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_WARNING, window, "Exit Go Bigger Boy?", message,
        static_cast<int>(buttons.size()), buttons.data(), nullptr,
    };
    auto selection = 0;
    return SDL_ShowMessageBox(&box, &selection) && selection == 1;
}

void show_help(SDL_Window* window) {
    const auto message = std::string("Version ") + GBB_VERSION + "\n\n" +
        "Space: Pause/resume\n"
        "Ctrl+R: Reset\n"
        "Ctrl+O: Open ROM\n"
        "Ctrl+L: Open game library\n"
        "Ctrl+K: Configure controls\n"
        "Ctrl+P: Choose display palette\n"
        "Ctrl+1 through Ctrl+9: Open recent ROM\n"
        "F5: Quick save\n"
        "F8: Load quick save\n"
        "F11: Toggle fullscreen\n"
        "F1: Show this help\n"
        "Escape: Quit\n\n"
        "Game Boy Printer pages are saved automatically as BMP images.\n"
        "Game Boy Camera cartridges use the first available webcam.\n"
        "Rumble cartridges vibrate the connected gamepad when supported.";
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_INFORMATION, "Go Bigger Boy (GBB) controls",
        message.c_str(), window));
}

void show_error(SDL_Window* window, const std::string& message);
#ifdef __ANDROID__
void open_android_library() noexcept;
#endif

void activate_dashboard_selection(
    const std::size_t selection, const std::vector<std::string>& recent,
    gameboy::Emulator* emulator, DialogState& dialog, SdlResources& sdl,
    const std::filesystem::path& preference_path,
    std::optional<std::string>& pending_rom, bool& dashboard_visible,
    std::size_t& display_palette, bool& running) {
    const auto items = dashboard_items(emulator != nullptr, recent);
    if (selection >= items.size()) return;
    const auto& item = items[selection];
    switch (item.action) {
    case DashboardAction::resume:
        dashboard_visible = false;
        break;
    case DashboardAction::open_rom:
        show_rom_dialog(dialog, sdl.window);
        break;
    case DashboardAction::palette:
        choose_display_palette(emulator, sdl, preference_path,
                               display_palette);
        break;
    case DashboardAction::recent_rom:
        if (item.recent_index < recent.size()) {
            pending_rom = recent[item.recent_index];
        }
        break;
    case DashboardAction::quit:
        if (confirm_exit(sdl.window)) running = false;
        break;
    }
}

std::optional<std::size_t> dashboard_row_at(
    const float logical_x, const float logical_y, const std::size_t selection,
    const std::size_t item_count) {
    if (logical_x < 9.0F || logical_x > 151.0F ||
        logical_y < dashboard_first_row_y) {
        return std::nullopt;
    }
    const auto row = static_cast<std::size_t>(
        (logical_y - dashboard_first_row_y) / dashboard_row_height);
    if (row >= dashboard_visible_rows) return std::nullopt;
    const auto index = dashboard_first_visible(selection, item_count) + row;
    return index < item_count ? std::optional<std::size_t>{index}
                              : std::nullopt;
}

void process_events(std::unique_ptr<gameboy::Emulator>& emulator,
                    SdlResources& sdl, DialogState& dialog,
                    const std::filesystem::path& preference_path,
                    InputBindings& bindings,
                    InputBindings& configuration_backup,
                    const std::vector<std::string>& recent,
                    const std::string& current_rom,
                    std::optional<BindingConfiguration>& configuring,
                    std::optional<std::string>& pending_rom,
                    std::size_t& display_palette, bool& dashboard_visible,
                    std::size_t& dashboard_selection, bool& paused,
                    bool& fullscreen, bool& reset_requested, bool& running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            running = false;
            break;
        case SDL_EVENT_DROP_FILE:
            if (event.drop.data != nullptr) pending_rom = event.drop.data;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
#ifdef __ANDROID__
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_AC_BACK) {
                if (dashboard_visible && emulator != nullptr) {
                    dashboard_visible = false;
                } else {
                    if (emulator) release_all_buttons(*emulator);
                    if (confirm_exit(sdl.window)) running = false;
                }
                break;
            }
#endif
            if (dashboard_visible) {
                if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) break;
                const auto item_count =
                    dashboard_items(emulator != nullptr, recent).size();
                dashboard_selection =
                    std::min(dashboard_selection, item_count - 1);
                if (event.key.key == SDLK_UP) {
                    dashboard_selection = dashboard_selection == 0
                                              ? item_count - 1
                                              : dashboard_selection - 1;
                } else if (event.key.key == SDLK_DOWN) {
                    dashboard_selection =
                        (dashboard_selection + 1) % item_count;
                } else if (event.key.key == SDLK_RETURN ||
                           event.key.key == SDLK_SPACE) {
                    activate_dashboard_selection(
                        dashboard_selection, recent, emulator.get(), dialog,
                        sdl, preference_path, pending_rom, dashboard_visible,
                        display_palette, running);
                } else if (event.key.key == SDLK_O) {
                    show_rom_dialog(dialog, sdl.window);
                } else if (event.key.key == SDLK_ESCAPE) {
                    if (emulator) {
                        dashboard_visible = false;
                    } else {
                        running = false;
                    }
                }
                break;
            }
            if (configuring) {
                if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) break;
                if (event.key.key == SDLK_ESCAPE) {
                    bindings = configuration_backup;
                    configuring.reset();
                    update_window_title(sdl.window, current_rom, paused,
                                        configuring);
                    break;
                }
                if (configuring->device != BindingDevice::keyboard) break;
                if (configuring->slot == 1 && event.key.key == SDLK_SPACE) {
                    bindings.keys[configuring->index][1] = SDLK_UNKNOWN;
                    configuring->slot = 0;
                    ++configuring->index;
                    if (configuring->index == bindings.keys.size()) {
                        configuring.reset();
                        save_bindings(preference_path, bindings);
                        static_cast<void>(SDL_ShowSimpleMessageBox(
                            SDL_MESSAGEBOX_INFORMATION, "Keyboard controls",
                            "Keyboard bindings saved.", sdl.window));
                    }
                    update_window_title(sdl.window, current_rom, paused,
                                        configuring);
                    break;
                }
                if (reserved_gameplay_key(event.key.key)) {
                    show_error(sdl.window,
                               "That key is reserved for an emulator shortcut.");
                    break;
                }
                bool duplicate = false;
                for (std::size_t index = 0; index < bindings.keys.size(); ++index) {
                    for (std::size_t slot = 0;
                         slot < bindings.keys[index].size(); ++slot) {
                        if (bindings.keys[index][slot] == event.key.key &&
                            (index != configuring->index ||
                             slot != configuring->slot)) {
                            duplicate = true;
                        }
                    }
                }
                if (duplicate) {
                    show_error(sdl.window, "That key is already assigned.");
                    break;
                }
                bindings.keys[configuring->index][configuring->slot] =
                    event.key.key;
                if (configuring->slot == 0) {
                    configuring->slot = 1;
                } else {
                    configuring->slot = 0;
                    ++configuring->index;
                    if (configuring->index == bindings.keys.size()) {
                        configuring.reset();
                        save_bindings(preference_path, bindings);
                        static_cast<void>(SDL_ShowSimpleMessageBox(
                            SDL_MESSAGEBOX_INFORMATION, "Keyboard controls",
                            "Keyboard bindings saved.", sdl.window));
                    }
                }
                update_window_title(sdl.window, current_rom, paused,
                                    configuring);
                break;
            }

            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_O && (event.key.mod & SDL_KMOD_CTRL) != 0) {
                show_rom_dialog(dialog, sdl.window);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_L &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                if (emulator) release_all_buttons(*emulator);
#ifdef __ANDROID__
                open_android_library();
#else
#ifdef _WIN32
                SDL_HideWindow(sdl.window);
#endif
                dashboard_visible = true;
                dashboard_selection = 0;
#endif
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
                    if (action == ControlsAction::gamepad &&
                        sdl.gamepad == nullptr) {
                        show_error(sdl.window,
                                   "Connect a gamepad before configuring it.");
                    } else {
                        begin_binding_configuration(
                            bindings, configuration_backup, configuring,
                            action == ControlsAction::keyboard
                                ? BindingDevice::keyboard
                                : BindingDevice::gamepad);
                    }
                }
                update_window_title(sdl.window, current_rom, paused,
                                    configuring);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_P &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                choose_display_palette(emulator.get(), sdl, preference_path,
                                       display_palette);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key >= SDLK_1 && event.key.key <= SDLK_9 &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                const auto index = static_cast<std::size_t>(event.key.key - SDLK_1);
                if (index < recent.size()) pending_rom = recent[index];
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_R &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0 && emulator) {
                reset_requested = true;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_SPACE && emulator) {
                paused = !paused;
                release_all_buttons(*emulator);
                update_window_title(sdl.window, current_rom, paused,
                                    configuring);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_F5 && emulator) {
                try {
                    save_quick_state(preference_path, *emulator);
                    static_cast<void>(SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_INFORMATION, "Quick save",
                        "State saved.", sdl.window));
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_F8 && emulator) {
                try {
                    load_quick_state(preference_path, *emulator);
                    release_all_buttons(*emulator);
                    if (sdl.audio_stream != nullptr) {
                        static_cast<void>(SDL_ClearAudioStream(sdl.audio_stream));
                    }
                    static_cast<void>(SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_INFORMATION, "Quick save",
                        "State loaded.", sdl.window));
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_F11) {
                fullscreen = !fullscreen;
                if (!SDL_SetWindowFullscreen(sdl.window, fullscreen)) {
                    fullscreen = !fullscreen;
                    show_error(sdl.window, SDL_GetError());
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_F1) {
                if (emulator) release_all_buttons(*emulator);
                show_help(sdl.window);
            } else if (event.key.key == SDLK_ESCAPE &&
                       event.type == SDL_EVENT_KEY_DOWN) {
                running = false;
            } else if (emulator && !event.key.repeat) {
                if (const auto button = keyboard_button(bindings, event.key.key)) {
                    emulator->set_button(*button,
                                         event.type == SDL_EVENT_KEY_DOWN);
                }
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                auto x = event.button.x;
                auto y = event.button.y;
                static_cast<void>(SDL_RenderCoordinatesFromWindow(
                    sdl.renderer, x, y, &x, &y));
                if (dashboard_visible) {
                    const auto item_count =
                        dashboard_items(emulator != nullptr, recent).size();
                    if (const auto selected = dashboard_row_at(
                            x, y, dashboard_selection, item_count)) {
                        dashboard_selection = *selected;
                        activate_dashboard_selection(
                            dashboard_selection, recent, emulator.get(), dialog,
                            sdl, preference_path, pending_rom,
                            dashboard_visible, display_palette, running);
                    }
                } else if (x < 20.0F && y < 17.0F) {
                    if (emulator) release_all_buttons(*emulator);
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
            }
            break;
#ifdef __ANDROID__
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP: {
            const auto finger = event.tfinger.fingerID;
            const auto [touch_x, touch_y] =
                logical_touch_position(event.tfinger, sdl);
            if (dashboard_visible) {
                if (event.type == SDL_EVENT_FINGER_UP) {
                    const auto item_count =
                        dashboard_items(emulator != nullptr, recent).size();
                    if (const auto selected = dashboard_row_at(
                            touch_x * gameboy::Ppu::screen_width,
                            touch_y * gameboy::Ppu::screen_height,
                            dashboard_selection, item_count)) {
                        dashboard_selection = *selected;
                        activate_dashboard_selection(
                            dashboard_selection, recent, emulator.get(), dialog,
                            sdl, preference_path, pending_rom,
                            dashboard_visible, display_palette, running);
                    }
                }
                break;
            }
            const auto existing = std::find_if(
                sdl.touches.begin(), sdl.touches.end(),
                [finger](const SdlResources::TouchPoint& point) {
                    return point.id == finger;
                });
            if (event.type == SDL_EVENT_FINGER_UP) {
                if (existing != sdl.touches.end()) sdl.touches.erase(existing);
            } else if (existing == sdl.touches.end()) {
                sdl.touches.push_back({finger, touch_x, touch_y});
            } else {
                existing->x = touch_x;
                existing->y = touch_y;
            }
            if (event.type == SDL_EVENT_FINGER_DOWN &&
                touch_x < 0.13F && touch_y < 0.16F) {
                clear_touch_buttons(emulator.get(), sdl);
                open_android_library();
            }
            refresh_touch_buttons(emulator.get(), sdl);
            break;
        }
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            clear_touch_buttons(emulator.get(), sdl);
            if (emulator) emulator->flush_battery();
            paused = true;
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            display_palette = load_display_palette(preference_path);
            if (emulator != nullptr) {
                emulator->set_dmg_compatibility_colors(
                    gameboy::display_palettes[display_palette]
                        .cgb_compatibility);
            }
            paused = false;
            break;
#endif
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (emulator) release_all_buttons(*emulator);
#ifdef __ANDROID__
            clear_touch_buttons(emulator.get(), sdl);
#endif
            stop_rumble(sdl);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (sdl.gamepad == nullptr) {
                sdl.gamepad = SDL_OpenGamepad(event.gdevice.which);
                sdl.rumble_warning_shown = false;
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (sdl.gamepad != nullptr &&
                SDL_GetGamepadID(sdl.gamepad) == event.gdevice.which) {
                SDL_CloseGamepad(sdl.gamepad);
                sdl.gamepad = nullptr;
                sdl.rumble_output_active = false;
                sdl.rumble_warning_shown = false;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (dashboard_visible) {
                if (event.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN) break;
                const auto item_count =
                    dashboard_items(emulator != nullptr, recent).size();
                dashboard_selection =
                    std::min(dashboard_selection, item_count - 1);
                const auto button = static_cast<SDL_GamepadButton>(
                    event.gbutton.button);
                if (button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                    dashboard_selection = dashboard_selection == 0
                                              ? item_count - 1
                                              : dashboard_selection - 1;
                } else if (button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                    dashboard_selection =
                        (dashboard_selection + 1) % item_count;
                } else if (button == SDL_GAMEPAD_BUTTON_SOUTH) {
                    activate_dashboard_selection(
                        dashboard_selection, recent, emulator.get(), dialog,
                        sdl, preference_path, pending_rom, dashboard_visible,
                        display_palette, running);
                } else if (button == SDL_GAMEPAD_BUTTON_EAST && emulator) {
                    dashboard_visible = false;
                }
            } else if (configuring &&
                configuring->device == BindingDevice::gamepad) {
                if (event.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN) break;
                const auto pressed = static_cast<SDL_GamepadButton>(
                    event.gbutton.button);
                const auto duplicate = std::find(
                    bindings.gamepad_buttons.begin(),
                    bindings.gamepad_buttons.end(), pressed);
                if (duplicate != bindings.gamepad_buttons.end() &&
                    static_cast<std::size_t>(
                        duplicate - bindings.gamepad_buttons.begin()) !=
                        configuring->index) {
                    show_error(sdl.window,
                               "That gamepad button is already assigned.");
                    break;
                }
                bindings.gamepad_buttons[configuring->index] = pressed;
                ++configuring->index;
                if (configuring->index == bindings.gamepad_buttons.size()) {
                    configuring.reset();
                    save_bindings(preference_path, bindings);
                    static_cast<void>(SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_INFORMATION, "Gamepad controls",
                        "Gamepad bindings saved.", sdl.window));
                }
                update_window_title(sdl.window, current_rom, paused,
                                    configuring);
            } else if (emulator) {
                if (const auto button = gamepad_button(bindings,
                                                       event.gbutton.button)) {
                    emulator->set_button(
                        *button, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                }
            }
            break;
        default:
            break;
        }
    }
}

void show_error(SDL_Window* window, const std::string& message) {
    std::cerr << "Error: " << message << '\n';
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR, "Go Bigger Boy (GBB)", message.c_str(), window));
}

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

void close_camera(SdlResources& sdl) noexcept {
    if (sdl.camera != nullptr) {
        SDL_CloseCamera(sdl.camera);
        sdl.camera = nullptr;
    }
    if (sdl.camera_frame != nullptr) {
        SDL_DestroySurface(sdl.camera_frame);
        sdl.camera_frame = nullptr;
    }
    sdl.next_camera_frame = {};
    sdl.camera_warning_shown = false;
}

#ifdef __ANDROID__
void open_android_library() noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) return;
    const auto activity_class = environment->GetObjectClass(activity);
    if (activity_class != nullptr) {
        const auto method = environment->GetMethodID(
            activity_class, "openLibrary", "()V");
        if (method != nullptr) environment->CallVoidMethod(activity, method);
        environment->DeleteLocalRef(activity_class);
    }
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    environment->DeleteLocalRef(activity);
}

std::optional<int> android_camera_orientation_correction_degrees() noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) return std::nullopt;

    static jmethodID orientation_method{};
    if (orientation_method == nullptr) {
        const auto activity_class = environment->GetObjectClass(activity);
        if (activity_class != nullptr) {
            orientation_method = environment->GetMethodID(
                activity_class, "getCameraOrientationCorrectionDegrees", "()I");
            environment->DeleteLocalRef(activity_class);
        }
    }
    std::optional<int> orientation;
    if (orientation_method != nullptr) {
        orientation = static_cast<int>(
            environment->CallIntMethod(activity, orientation_method));
    }
    if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
        orientation.reset();
    }
    environment->DeleteLocalRef(activity);
    return orientation;
}
#endif

void configure_camera(SdlResources& sdl, const gameboy::Emulator& emulator) {
    close_camera(sdl);
    if (!emulator.has_camera()) return;

    if (!SDL_InitSubSystem(SDL_INIT_CAMERA)) {
        std::cerr << "Warning: camera subsystem is unavailable: "
                  << SDL_GetError() << '\n';
        sdl.camera_warning_shown = true;
        return;
    }
    int count = 0;
    SDL_CameraID* cameras = SDL_GetCameras(&count);
    if (cameras == nullptr || count == 0) {
        SDL_free(cameras);
        std::cerr << "Warning: no webcam was found; the Game Boy Camera "
                     "will use its fallback image.\n";
        sdl.camera_warning_shown = true;
        return;
    }

    const auto camera_id = cameras[0];
    const auto camera_position = SDL_GetCameraPosition(camera_id);
    sdl.mirror_camera =
        camera_position == SDL_CAMERA_POSITION_FRONT_FACING;
    sdl.camera_back_facing =
        camera_position == SDL_CAMERA_POSITION_BACK_FACING;
    constexpr SDL_CameraSpec camera_spec{
        SDL_PIXELFORMAT_RGBA32,
        SDL_COLORSPACE_SRGB,
        static_cast<int>(gameboy::Cartridge::camera_width),
        static_cast<int>(gameboy::Cartridge::camera_height),
        15,
        1,
    };
    sdl.camera = SDL_OpenCamera(camera_id, &camera_spec);
    SDL_free(cameras);
    if (sdl.camera == nullptr) {
        std::cerr << "Warning: webcam could not be opened: " << SDL_GetError()
                  << '\n';
        sdl.camera_warning_shown = true;
        return;
    }
    sdl.camera_frame = SDL_CreateSurface(
        static_cast<int>(gameboy::Cartridge::camera_width),
        static_cast<int>(gameboy::Cartridge::camera_height),
        SDL_PIXELFORMAT_RGBA32);
    if (sdl.camera_frame == nullptr) {
        std::cerr << "Warning: webcam conversion surface could not be created: "
                  << SDL_GetError() << '\n';
        close_camera(sdl);
        sdl.camera_warning_shown = true;
    }
}

void update_camera_frame(gameboy::Emulator* emulator, SdlResources& sdl) {
    if (emulator == nullptr || !emulator->has_camera() || sdl.camera == nullptr) {
        return;
    }
    const auto permission = SDL_GetCameraPermissionState(sdl.camera);
    if (permission == SDL_CAMERA_PERMISSION_STATE_DENIED) {
        if (!sdl.camera_warning_shown) {
            std::cerr << "Warning: webcam permission was denied; the Game Boy "
                         "Camera will use its fallback image.\n";
            sdl.camera_warning_shown = true;
        }
        return;
    }
    if (permission != SDL_CAMERA_PERMISSION_STATE_APPROVED) return;

    const auto now = std::chrono::steady_clock::now();
    if (now < sdl.next_camera_frame) return;
    sdl.next_camera_frame = now + std::chrono::milliseconds(66);

    SDL_Surface* source = SDL_AcquireCameraFrame(sdl.camera, nullptr);
    if (source == nullptr) return;
    auto rotation_degrees = SDL_GetFloatProperty(
        SDL_GetSurfaceProperties(source), SDL_PROP_SURFACE_ROTATION_FLOAT, 0.0F);
#ifdef __ANDROID__
    if (const auto correction =
            android_camera_orientation_correction_degrees()) {
        rotation_degrees += static_cast<float>(
            sdl.camera_back_facing ? -*correction : *correction);
    }
#endif
    auto rotation_quarters = static_cast<int>(
        rotation_degrees / 90.0F + (rotation_degrees < 0.0F ? -0.5F : 0.5F));
    rotation_quarters = (rotation_quarters % 4 + 4) % 4;
    constexpr auto target_width = gameboy::Cartridge::camera_width;
    constexpr auto target_height = gameboy::Cartridge::camera_height;
    SDL_Surface* frame = source;
    if (source->format != SDL_PIXELFORMAT_RGBA32 ||
        source->w != static_cast<int>(target_width) ||
        source->h != static_cast<int>(target_height)) {
        SDL_Rect crop{0, 0, source->w, source->h};
        if (static_cast<std::int64_t>(source->w) * target_height >
            static_cast<std::int64_t>(source->h) * target_width) {
            crop.w = static_cast<int>(static_cast<std::int64_t>(source->h) *
                                      target_width / target_height);
            crop.x = (source->w - crop.w) / 2;
        } else {
            crop.h = static_cast<int>(static_cast<std::int64_t>(source->w) *
                                      target_height / target_width);
            crop.y = (source->h - crop.h) / 2;
        }
        if (!SDL_BlitSurfaceScaled(source, &crop, sdl.camera_frame, nullptr,
                                   SDL_SCALEMODE_NEAREST)) {
            SDL_ReleaseCameraFrame(sdl.camera, source);
            if (!sdl.camera_warning_shown) {
                std::cerr << "Warning: webcam frame conversion failed: "
                          << SDL_GetError() << '\n';
                sdl.camera_warning_shown = true;
            }
            return;
        }
        frame = sdl.camera_frame;
    }

    const auto needs_lock = SDL_MUSTLOCK(frame);
    if (needs_lock && !SDL_LockSurface(frame)) {
        SDL_ReleaseCameraFrame(sdl.camera, source);
        return;
    }
    std::array<std::uint8_t, target_width * target_height> grayscale{};
    const auto* pixels = static_cast<const std::uint8_t*>(frame->pixels);
    const auto rotated_width = rotation_quarters % 2 == 0 ? frame->w : frame->h;
    const auto rotated_height = rotation_quarters % 2 == 0 ? frame->h : frame->w;
    auto crop_x = 0;
    auto crop_y = 0;
    auto crop_width = rotated_width;
    auto crop_height = rotated_height;
    if (static_cast<std::int64_t>(rotated_width) * target_height >
        static_cast<std::int64_t>(rotated_height) * target_width) {
        crop_width = static_cast<int>(
            static_cast<std::int64_t>(rotated_height) * target_width /
            target_height);
        crop_x = (rotated_width - crop_width) / 2;
    } else {
        crop_height = static_cast<int>(
            static_cast<std::int64_t>(rotated_width) * target_height /
            target_width);
        crop_y = (rotated_height - crop_height) / 2;
    }
    for (std::size_t y = 0; y < target_height; ++y) {
        for (std::size_t x = 0; x < target_width; ++x) {
            auto rotated_x = crop_x + static_cast<int>(
                (x * 2 + 1) * static_cast<std::size_t>(crop_width) /
                (target_width * 2));
            const auto rotated_y = crop_y + static_cast<int>(
                (y * 2 + 1) * static_cast<std::size_t>(crop_height) /
                (target_height * 2));
            if (sdl.mirror_camera) {
                rotated_x = rotated_width - 1 - rotated_x;
            }
            auto sample_x = rotated_x;
            auto sample_y = rotated_y;
            switch (rotation_quarters) {
            case 1:
                sample_x = rotated_y;
                sample_y = frame->h - 1 - rotated_x;
                break;
            case 2:
                sample_x = frame->w - 1 - rotated_x;
                sample_y = frame->h - 1 - rotated_y;
                break;
            case 3:
                sample_x = frame->w - 1 - rotated_y;
                sample_y = rotated_x;
                break;
            default: break;
            }
            const auto* pixel = pixels + sample_y * frame->pitch + sample_x * 4;
            grayscale[y * target_width + x] = static_cast<std::uint8_t>(
                (77U * pixel[0] + 150U * pixel[1] + 29U * pixel[2]) >> 8);
        }
    }
    if (needs_lock) SDL_UnlockSurface(frame);
    SDL_ReleaseCameraFrame(sdl.camera, source);
    emulator->set_camera_frame(grayscale.data(), grayscale.size());
}

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

void load_rom(const std::string& path,
              std::unique_ptr<gameboy::Emulator>& emulator,
              const gameboy::DisplayPalette& palette, SdlResources& sdl,
              const std::filesystem::path& preference_path) {
#ifdef __ANDROID__
    std::size_t byte_count{};
    void* loaded = SDL_LoadFile(path.c_str(), &byte_count);
    if (loaded == nullptr) {
        throw std::runtime_error(std::string{"Could not read ROM: "} +
                                 SDL_GetError());
    }
    const std::unique_ptr<void, decltype(&SDL_free)> owned(loaded, SDL_free);
    const auto* begin = static_cast<const std::uint8_t*>(loaded);
    std::vector<std::uint8_t> bytes(begin, begin + byte_count);
    gameboy::Cartridge cartridge(std::move(bytes));
    if (cartridge.has_battery() && !preference_path.empty()) {
        const auto save_directory = preference_path / "saves";
        std::filesystem::create_directories(save_directory);
        std::ostringstream name;
        name << std::hex << std::setw(16) << std::setfill('0')
             << cartridge.rom_fingerprint() << ".gb";
        cartridge.set_persistence_path(save_directory / name.str());
    }
    auto replacement = std::make_unique<gameboy::Emulator>(
        std::move(cartridge));
#else
    static_cast<void>(preference_path);
    auto replacement = std::make_unique<gameboy::Emulator>(
        gameboy::Cartridge::from_file(std::filesystem::u8path(path)));
#endif
    replacement->bus().connect_printer();
    replacement->set_dmg_compatibility_colors(palette.cgb_compatibility);
    stop_rumble(sdl);
    if (emulator) emulator->flush_battery();
    emulator = std::move(replacement);
    configure_camera(sdl, *emulator);
}

void present_menu_button(SdlResources& sdl) {
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_BLEND));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 220, 235, 220, 100));
    const SDL_FRect button{3, 3, 15, 11};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &button));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 16, 20, 16, 150));
    const std::array<SDL_FRect, 3> menu_lines{{
        {6, 5, 9, 1.5F}, {6, 8, 9, 1.5F}, {6, 11, 9, 1.5F}}};
    for (const auto& line : menu_lines) {
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &line));
    }
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_NONE));
}

#ifdef __ANDROID__
void present_touch_controls(SdlResources& sdl) {
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_BLEND));
    const auto draw = [&sdl](const SDL_FRect& rect, const bool pressed) {
        static_cast<void>(SDL_SetRenderDrawColor(
            sdl.renderer, pressed ? 139 : 220, pressed ? 207 : 235,
            pressed ? 105 : 220, pressed ? 190 : 100));
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &rect));
    };

    draw({24, 90, 16, 42}, sdl.touch_buttons[1] || sdl.touch_buttons[0]);
    draw({11, 103, 42, 16}, sdl.touch_buttons[2] || sdl.touch_buttons[3]);
    draw({126, 91, 22, 22}, sdl.touch_buttons[4]);
    draw({103, 108, 22, 22}, sdl.touch_buttons[5]);
    draw({83, 130, 18, 7}, sdl.touch_buttons[7]);
    draw({61, 130, 18, 7}, sdl.touch_buttons[6]);
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_NONE));
}
#endif

#ifndef _WIN32
void present_dashboard(SdlResources& sdl,
                       const std::vector<std::string>& recent,
                       const bool can_resume, std::size_t& selection) {
    const auto items = dashboard_items(can_resume, recent);
    selection = std::min(selection, items.size() - 1);
    const auto first = dashboard_first_visible(selection, items.size());
    const auto visible = std::min(dashboard_visible_rows, items.size() - first);

    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 139, 172, 95, 255));
    const SDL_FRect header{0, 0, 160, 34};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &header));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 20, 40, 28, 255));
    static_cast<void>(SDL_RenderDebugText(sdl.renderer, 28, 5,
                                          "GO BIGGER BOY"));
    static_cast<void>(SDL_RenderDebugText(sdl.renderer, 12, 18,
                                          "YOUR GAME LIBRARY"));
    static_cast<void>(SDL_RenderLine(sdl.renderer, 9, 32, 151, 32));

    for (std::size_t row = 0; row < visible; ++row) {
        const auto index = first + row;
        const auto selected = index == selection;
        const auto y = dashboard_first_row_y +
                       static_cast<float>(row) * dashboard_row_height;
        const SDL_FRect card{9, y, 142, 15};
        static_cast<void>(SDL_SetRenderDrawColor(
            sdl.renderer, selected ? 36 : 201, selected ? 67 : 220,
            selected ? 46 : 174, 255));
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &card));
        static_cast<void>(SDL_SetRenderDrawColor(
            sdl.renderer, selected ? 232 : 35, selected ? 242 : 58,
            selected ? 205 : 40, 255));
        const auto label = std::string(selected ? "> " : "  ") +
                           dashboard_text(items[index].label, 14);
        static_cast<void>(SDL_RenderDebugText(sdl.renderer, 13, y + 3,
                                              label.c_str()));
    }

    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 35, 58, 40, 255));
    if (first > 0) {
        static_cast<void>(SDL_RenderDebugText(sdl.renderer, 153, 39, "^"));
    }
    if (first + visible < items.size()) {
        static_cast<void>(SDL_RenderDebugText(sdl.renderer, 153, 111, "v"));
    }
    static_cast<void>(SDL_RenderDebugText(sdl.renderer, 20, 134,
                                          "UP/DOWN + ENTER"));
}
#endif

void present(const gameboy::Emulator* emulator, SdlResources& sdl,
             const gameboy::DisplayPalette& palette,
             const std::vector<std::string>& recent,
             const bool dashboard_visible,
             std::size_t& dashboard_selection) {
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 16, 20, 16, 255));
    if (!SDL_RenderClear(sdl.renderer)) {
        sdl_error("Could not clear framebuffer");
    }
    if (dashboard_visible) {
#ifndef _WIN32
        present_dashboard(sdl, recent, emulator != nullptr,
                          dashboard_selection);
#else
        static_cast<void>(recent);
        static_cast<void>(dashboard_selection);
#endif
    } else if (emulator != nullptr) {
        const auto& pixels = emulator->framebuffer();
        const auto native_colors =
            emulator->bus().cgb_mode() || palette.cgb_compatibility;
        gameboy::Ppu::Framebuffer colored_pixels{};
        std::transform(pixels.begin(), pixels.end(), colored_pixels.begin(),
                       [&palette, native_colors](const std::uint32_t pixel) {
                           return native_colors
                                      ? pixel
                                      : gameboy::apply_display_palette(pixel,
                                                                       palette);
                       });
        if (!SDL_UpdateTexture(sdl.texture, nullptr, colored_pixels.data(),
                               static_cast<int>(gameboy::Ppu::screen_width *
                                                sizeof(std::uint32_t))) ||
            !SDL_RenderTexture(sdl.renderer, sdl.texture, nullptr, nullptr)) {
            sdl_error("Could not present framebuffer");
        }
    }
#ifdef __ANDROID__
    if (!dashboard_visible) present_touch_controls(sdl);
#endif
    if (!dashboard_visible) present_menu_button(sdl);
    if (!SDL_RenderPresent(sdl.renderer)) {
        sdl_error("Could not present framebuffer");
    }
}

void submit_audio(gameboy::Emulator* emulator, SdlResources& sdl) {
    if (emulator == nullptr) return;
    const auto samples = emulator->take_audio_samples();
    if (sdl.audio_stream != nullptr && !samples.empty() &&
        !SDL_PutAudioStreamData(
            sdl.audio_stream, samples.data(),
            static_cast<int>(samples.size() * sizeof(samples.front())))) {
        sdl_error("Could not queue audio samples");
    }
}

} // namespace

#ifdef __ANDROID__
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeLibraryEntries(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory = environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return nullptr;
    const auto library = gameboy::RomLibrary::load(
        std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);

    const auto string_class = environment->FindClass("java/lang/String");
    const auto& entries = library.entries();
    auto result = environment->NewObjectArray(
        static_cast<jsize>(entries.size()), string_class, nullptr);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        std::ostringstream encoded;
        encoded << std::hex << entry.metadata.fingerprint << '\x1f'
                << entry.metadata.crc32 << '\x1f'
                << entry.path.u8string() << '\x1f'
                << entry.metadata.title << '\x1f'
                << gameboy::platform_name(entry.metadata.platform) << '\x1f'
                << entry.metadata.language << '\x1f'
                << gameboy::cover_system_name(entry.metadata.platform) << '\x1f'
                << entry.metadata.cover_name << '\x1f'
                << std::dec << entry.last_played;
        const auto text = encoded.str();
        const auto value = environment->NewStringUTF(text.c_str());
        environment->SetObjectArrayElement(
            result, static_cast<jsize>(index), value);
        environment->DeleteLocalRef(value);
    }
    environment->DeleteLocalRef(string_class);
    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeRemoveLibraryEntry(
    JNIEnv* environment, jclass, jstring directory, jstring fingerprint) {
    const auto* raw_directory = environment->GetStringUTFChars(directory, nullptr);
    const auto* raw_fingerprint =
        environment->GetStringUTFChars(fingerprint, nullptr);
    if (raw_directory == nullptr || raw_fingerprint == nullptr) {
        if (raw_directory != nullptr) {
            environment->ReleaseStringUTFChars(directory, raw_directory);
        }
        if (raw_fingerprint != nullptr) {
            environment->ReleaseStringUTFChars(fingerprint, raw_fingerprint);
        }
        return JNI_FALSE;
    }
    auto library = gameboy::RomLibrary::load(
        std::filesystem::u8path(raw_directory));
    std::uint64_t value{};
    std::istringstream parser(raw_fingerprint);
    parser >> std::hex >> value;
    const auto removed = parser && library.remove(value);
    if (removed) library.save(std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    environment->ReleaseStringUTFChars(fingerprint, raw_fingerprint);
    return removed ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_GbbActivity_nativeOpenRom(
    JNIEnv* environment, jclass, jstring rom, jstring display_name) {
    const auto* raw_rom = environment->GetStringUTFChars(rom, nullptr);
    if (raw_rom == nullptr) return;
    const auto* raw_name = display_name == nullptr
                               ? nullptr
                               : environment->GetStringUTFChars(display_name,
                                                                 nullptr);
    {
        std::lock_guard<std::mutex> lock(android_rom_request_mutex);
        android_rom_request = AndroidRomRequest{
            raw_rom, raw_name == nullptr ? std::string{} : std::string{raw_name}};
    }
    if (raw_name != nullptr) {
        environment->ReleaseStringUTFChars(display_name, raw_name);
    }
    environment->ReleaseStringUTFChars(rom, raw_rom);
}
#endif

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << "Go Bigger Boy " GBB_VERSION << '\n';
            return EXIT_SUCCESS;
        }
        DialogState dialog;
        SdlResources sdl;
        const auto preference_path = preference_directory();
        restore_game_window_geometry(sdl.window, preference_path);
        auto rom_library = load_rom_library(preference_path);
        auto recent_roms = recent_paths(rom_library);
        auto bindings = load_bindings(preference_path);
        auto configuration_backup = bindings;
        auto display_palette = load_display_palette(preference_path);
        std::unique_ptr<gameboy::Emulator> emulator;
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
        bool update_check_complete = false;
#endif
#ifdef _WIN32
        bool reveal_sdl_after_present = false;
        bool return_to_dashboard_after_configuration = false;
#endif
        auto paused = false;
        auto fullscreen = false;
        auto reset_requested = false;
        auto running = true;
#ifdef __ANDROID__
        auto dashboard_visible = argc < 2;
#else
        auto dashboard_visible = argc != 2;
#endif
        std::size_t dashboard_selection = 0;
        std::uint64_t print_sequence = 0;

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
#ifdef _WIN32
        if (dashboard_visible) SDL_HideWindow(sdl.window);
#endif

        using Clock = std::chrono::steady_clock;
        constexpr auto cycles_per_frame = 70224U;
        const auto frame_duration = std::chrono::duration<double>(
            static_cast<double>(cycles_per_frame) / 4194304.0);
        auto next_frame = Clock::now();

        while (running) {
#ifndef __ANDROID__
            if (!update_check_complete) {
                std::string update_error;
                std::optional<gbb_desktop::UpdateInfo> update_result;
                if (update_checker.take_result(update_result, update_error)) {
                    update_check_complete = true;
                    if (!update_error.empty()) {
                        std::cerr << "Warning: update check unavailable: "
                                  << update_error << '\n';
                    }
                    available_update = std::move(update_result);
                }
            }
#endif
#ifdef _WIN32
            if (dashboard_visible && update_check_complete &&
                !available_update && !update_download) {
                save_game_window_geometry(sdl.window, preference_path);
                SDL_HideWindow(sdl.window);
                const auto result = gbb_desktop::show_windows_dashboard(
                    nullptr, rom_library, emulator != nullptr, display_palette,
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
                    if (emulator) {
                        emulator->set_dmg_compatibility_colors(
                            gameboy::display_palettes[display_palette]
                                .cgb_compatibility);
                    }
                }
                switch (result.action) {
                case gbb_desktop::DashboardResultAction::open_rom:
                    pending_rom = result.rom_path;
                    dashboard_visible = false;
                    break;
                case gbb_desktop::DashboardResultAction::resume:
                    dashboard_visible = false;
                    reveal_sdl_after_present = true;
                    break;
                case gbb_desktop::DashboardResultAction::configure_controls: {
                    const auto action = show_controls_dialog(sdl.window,
                                                             bindings);
                    if (action == ControlsAction::reset) {
                        bindings = InputBindings{};
                        save_bindings(preference_path, bindings);
                        dashboard_visible = true;
                    } else if (action == ControlsAction::keyboard ||
                               action == ControlsAction::gamepad) {
                        if (action == ControlsAction::gamepad &&
                            sdl.gamepad == nullptr) {
                            show_error(
                                sdl.window,
                                "Connect a gamepad before configuring it.");
                            dashboard_visible = true;
                        } else {
                            begin_binding_configuration(
                                bindings, configuration_backup, configuring,
                                action == ControlsAction::keyboard
                                    ? BindingDevice::keyboard
                                    : BindingDevice::gamepad);
                            dashboard_visible = false;
                            return_to_dashboard_after_configuration = true;
                            update_window_title(sdl.window, current_rom, paused,
                                                configuring);
                            SDL_ShowWindow(sdl.window);
                        }
                    } else {
                        dashboard_visible = true;
                    }
                    break;
                }
                case gbb_desktop::DashboardResultAction::quit:
                    running = false;
                    break;
                }
                if (!running) break;
            }
#endif
#ifdef __ANDROID__
            if (auto requested = take_android_rom_request()) {
                pending_rom = std::move(requested->path);
                pending_rom_name = std::move(requested->display_name);
            }
#endif
            const bool was_configuring = configuring.has_value();
            process_events(emulator, sdl, dialog, preference_path, bindings,
                           configuration_backup, recent_roms, current_rom,
                           configuring, pending_rom, display_palette,
                           dashboard_visible, dashboard_selection, paused,
                           fullscreen,
                           reset_requested, running);
#ifdef _WIN32
            if (return_to_dashboard_after_configuration && was_configuring &&
                !configuring) {
                return_to_dashboard_after_configuration = false;
                dashboard_visible = true;
                dashboard_selection = 0;
                SDL_HideWindow(sdl.window);
            }
#endif

            std::optional<std::string> dialog_error;
            collect_dialog_result(dialog, pending_rom, dialog_error);
            if (dialog_error) show_error(sdl.window, *dialog_error);

            if (reset_requested) {
                if (!current_rom.empty()) pending_rom = current_rom;
                reset_requested = false;
            }

            if (pending_rom) {
                try {
                    auto requested_rom = *pending_rom;
#ifdef __ANDROID__
                    requested_rom =
                        persist_android_rom(requested_rom, preference_path,
                                            pending_rom_name);
#endif
                    const bool reopening_current =
                        emulator && requested_rom == current_rom;
                    load_rom(requested_rom, emulator,
                             gameboy::display_palettes[display_palette], sdl,
                             preference_path);
                    if (sdl.audio_stream != nullptr) {
                        static_cast<void>(SDL_ClearAudioStream(sdl.audio_stream));
                    }
                    current_rom = requested_rom;
                    if (!reopening_current) paused = false;
                    dashboard_visible = false;
#ifdef _WIN32
                    reveal_sdl_after_present = true;
#endif
                    rom_library.remember(
                        current_rom, gameboy::inspect_rom_file(current_rom));
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
                        dashboard_visible = true;
                        dashboard_selection = 0;
                    }
                }
                pending_rom.reset();
#ifdef __ANDROID__
                pending_rom_name.clear();
#endif
                next_frame = Clock::now();
            }

#ifndef __ANDROID__
            if (available_update && !dialog_active(dialog) && !configuring &&
                !pending_rom) {
                if (offer_update(*available_update, emulator.get(), sdl)) {
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
                        static_cast<void>(SDL_SetWindowTitle(
                            sdl.window,
                            "Go Bigger Boy (GBB) - Downloading update..."));
                    } catch (const std::exception& error) {
                        show_error(sdl.window, error.what());
                    }
                }
                available_update.reset();
                next_frame = Clock::now();
            }

            if (update_download) {
                std::optional<gbb_desktop::DownloadedUpdate> downloaded;
                std::string download_error;
                if (update_download->take_result(downloaded, download_error)) {
                    update_download.reset();
                    if (!download_error.empty() || !downloaded) {
                        show_error(sdl.window,
                                   download_error.empty()
                                       ? "The update download failed."
                                       : download_error);
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

            update_camera_frame(emulator.get(), sdl);

            if (emulator && !paused && !dashboard_visible && !configuring &&
                !dialog_active(dialog)) {
                unsigned cycles = 0;
                while (running && cycles < cycles_per_frame &&
                       !emulator->frame_ready()) {
                    cycles += emulator->step();
                }
                if (emulator->frame_ready()) emulator->consume_frame();
            }
            update_rumble(emulator.get(), sdl,
                          !paused && !dashboard_visible && !configuring &&
                              !dialog_active(dialog));
            submit_audio(emulator.get(), sdl);
            try {
                save_completed_prints(emulator.get(), sdl.window,
                                      preference_path, current_rom,
                                      print_sequence);
            } catch (const std::exception& error) {
                show_error(sdl.window, error.what());
            }
            present(emulator.get(), sdl,
                    gameboy::display_palettes[display_palette], recent_roms,
                    dashboard_visible, dashboard_selection);
#ifdef _WIN32
            if (reveal_sdl_after_present && !dashboard_visible) {
                SDL_ShowWindow(sdl.window);
                reveal_sdl_after_present = false;
            }
#endif

            next_frame += std::chrono::duration_cast<Clock::duration>(frame_duration);
            const auto now = Clock::now();
            if (next_frame > now) {
                std::this_thread::sleep_until(next_frame);
            } else if (now - next_frame > std::chrono::milliseconds(100)) {
                next_frame = now;
            }
        }
        save_game_window_geometry(sdl.window, preference_path);
        if (emulator) emulator->flush_battery();
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
