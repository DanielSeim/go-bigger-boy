#include "gameboy/emulator.hpp"
#include "gameboy/display_palette.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <chrono>
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
#define NOMINMAX
#include <windows.h>
#endif

namespace {

[[noreturn]] void sdl_error(const std::string& action) {
    throw std::runtime_error(action + ": " + SDL_GetError());
}

class SdlResources {
public:
    SdlResources() {
        if (!SDL_SetAppMetadata("Go Bigger Boy (GBB)", "0.1.0",
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
        if (gamepad != nullptr) SDL_CloseGamepad(gamepad);
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
    std::array<SDL_Keycode, 8> keys{
        SDLK_RIGHT, SDLK_LEFT, SDLK_UP, SDLK_DOWN,
        SDLK_X, SDLK_Z, SDLK_BACKSPACE, SDLK_RETURN,
    };
    std::array<SDL_GamepadButton, 8> gamepad_buttons{
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
    };
};

enum class BindingDevice { keyboard, gamepad };

struct BindingConfiguration {
    BindingDevice device{};
    std::size_t index{};
};

constexpr std::uintmax_t maximum_quick_state_size = 2 * 1024 * 1024;

std::filesystem::path preference_directory() {
    char* raw_path = SDL_GetPrefPath("Go Bigger Boy", "GBB");
    if (raw_path == nullptr) return {};
    const auto path = std::filesystem::u8path(raw_path);
    SDL_free(raw_path);
    return path;
}

std::vector<std::string> load_recent_roms(const std::filesystem::path& directory) {
    std::vector<std::string> paths;
    if (directory.empty()) return paths;
    std::ifstream input(directory / "recent-roms.txt");
    std::string path;
    while (paths.size() < 9 && std::getline(input, path)) {
        if (!path.empty()) paths.push_back(path);
    }
    return paths;
}

void save_recent_roms(const std::filesystem::path& directory,
                      const std::vector<std::string>& paths) {
    if (directory.empty()) return;
    std::ofstream output(directory / "recent-roms.txt", std::ios::trunc);
    for (const auto& path : paths) output << path << '\n';
}

void remember_rom(const std::string& path, std::vector<std::string>& recent,
                  const std::filesystem::path& directory) {
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > 9) recent.resize(9);
    save_recent_roms(directory, recent);
}

InputBindings load_bindings(const std::filesystem::path& directory) {
    InputBindings bindings;
    if (directory.empty()) return bindings;
    std::ifstream input(directory / "controls.txt");
    auto loaded = bindings.keys;
    for (auto& key : loaded) {
        long long value = 0;
        if (!(input >> value)) return bindings;
        key = static_cast<SDL_Keycode>(value);
    }
    if (std::find(loaded.begin(), loaded.end(), SDLK_UNKNOWN) != loaded.end()) {
        return bindings;
    }
    auto unique_keys = loaded;
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

void save_bindings(const std::filesystem::path& directory,
                   const InputBindings& bindings) {
    if (directory.empty()) return;
    std::ofstream output(directory / "controls.txt", std::ios::trunc);
    for (const auto key : bindings.keys) {
        output << static_cast<long long>(key) << '\n';
    }
    for (const auto button : bindings.gamepad_buttons) {
        output << static_cast<int>(button) << '\n';
    }
}

std::size_t load_display_palette(const std::filesystem::path& directory) {
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

void save_display_palette(const std::filesystem::path& directory,
                          const std::size_t palette) {
    if (directory.empty() || palette >= gameboy::display_palettes.size()) return;
    std::ofstream output(directory / "palette.txt", std::ios::trunc);
    output << gameboy::display_palettes[palette].id << '\n';
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
        if (bindings.keys[index] == key) return button_order[index];
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

void update_window_title(SDL_Window* window, const std::string& current_rom,
                         const bool paused,
                         const std::optional<BindingConfiguration>& configuring) {
    std::string title = "Go Bigger Boy (GBB)";
    if (configuring) {
        title += configuring->device == BindingDevice::keyboard
                     ? " - Press a key for "
                     : " - Press a gamepad button for ";
        title += button_names[configuring->index];
        title += " (Esc: cancel)";
    } else {
        if (!current_rom.empty()) {
            title += " - " +
                     std::filesystem::u8path(current_rom).filename().u8string();
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

ControlsAction show_controls_dialog(SDL_Window* window,
                                    const InputBindings& bindings) {
    std::ostringstream message;
    message << "Current controls:\n";
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        message << button_names[index] << ": "
                << SDL_GetKeyName(bindings.keys[index]) << " / "
                << SDL_GetGamepadStringForButton(
                       bindings.gamepad_buttons[index])
                << '\n';
    }
    message << "\nChoose which controls to configure.";

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

std::optional<std::string> show_recent_dialog(
    SDL_Window* window, const std::vector<std::string>& recent) {
    if (recent.empty()) {
        static_cast<void>(SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_INFORMATION, "Recent ROMs",
            "No ROMs have been opened yet.", window));
        return std::nullopt;
    }

    std::vector<std::string> labels;
    std::vector<SDL_MessageBoxButtonData> buttons;
    labels.reserve(recent.size() + 1);
    buttons.reserve(recent.size() + 1);
    for (std::size_t index = 0; index < recent.size(); ++index) {
        labels.push_back(std::to_string(index + 1) + ". " +
                         std::filesystem::u8path(recent[index])
                             .filename().u8string());
    }
    labels.emplace_back("Cancel");
    for (std::size_t index = 0; index < recent.size(); ++index) {
        buttons.push_back({0, static_cast<int>(index), labels[index].c_str()});
    }
    buttons.push_back({SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, -1,
                       labels.back().c_str()});

    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_INFORMATION, window, "Recent ROMs",
        "Choose a ROM to open:", static_cast<int>(buttons.size()),
        buttons.data(), nullptr,
    };
    auto selection = -1;
    if (!SDL_ShowMessageBox(&box, &selection) || selection < 0 ||
        static_cast<std::size_t>(selection) >= recent.size()) {
        return std::nullopt;
    }
    return recent[static_cast<std::size_t>(selection)];
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

void show_help(SDL_Window* window) {
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_INFORMATION, "Go Bigger Boy (GBB) controls",
        "Space: Pause/resume\n"
        "Ctrl+R: Reset\n"
        "Ctrl+O: Open ROM\n"
        "Ctrl+L: Recent ROMs\n"
        "Ctrl+K: Configure controls\n"
        "Ctrl+P: Choose display palette\n"
        "Ctrl+1 through Ctrl+9: Open recent ROM\n"
        "F5: Quick save\n"
        "F8: Load quick save\n"
        "F11: Toggle fullscreen\n"
        "F1: Show this help\n"
        "Escape: Quit",
        window));
}

void show_error(SDL_Window* window, const std::string& message);

void process_events(std::unique_ptr<gameboy::Emulator>& emulator,
                    SdlResources& sdl, DialogState& dialog,
                    const std::filesystem::path& preference_path,
                    InputBindings& bindings,
                    InputBindings& configuration_backup,
                    const std::vector<std::string>& recent,
                    const std::string& current_rom,
                    std::optional<BindingConfiguration>& configuring,
                    std::optional<std::string>& pending_rom,
                    std::size_t& display_palette, bool& paused, bool& fullscreen,
                    bool& reset_requested, bool& running) {
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
                if (reserved_gameplay_key(event.key.key)) {
                    show_error(sdl.window,
                               "That key is reserved for an emulator shortcut.");
                    break;
                }
                const auto duplicate = std::find(bindings.keys.begin(),
                                                 bindings.keys.end(),
                                                 event.key.key);
                if (duplicate != bindings.keys.end() &&
                    static_cast<std::size_t>(duplicate - bindings.keys.begin()) !=
                        configuring->index) {
                    show_error(sdl.window, "That key is already assigned.");
                    break;
                }
                bindings.keys[configuring->index] = event.key.key;
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

            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_O && (event.key.mod & SDL_KMOD_CTRL) != 0) {
                show_rom_dialog(dialog, sdl.window);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_L &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                if (emulator) release_all_buttons(*emulator);
                if (const auto selected = show_recent_dialog(sdl.window, recent)) {
                    pending_rom = *selected;
                }
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
                        configuration_backup = bindings;
                        configuring = BindingConfiguration{
                            action == ControlsAction::keyboard
                                ? BindingDevice::keyboard
                                : BindingDevice::gamepad,
                            0};
                    }
                }
                update_window_title(sdl.window, current_rom, paused,
                                    configuring);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_P &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                if (emulator) release_all_buttons(*emulator);
                if (const auto selected =
                        show_palette_dialog(sdl.window, display_palette)) {
                    display_palette = *selected;
                    save_display_palette(preference_path, display_palette);
                }
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
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (emulator) release_all_buttons(*emulator);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (sdl.gamepad == nullptr) {
                sdl.gamepad = SDL_OpenGamepad(event.gdevice.which);
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (sdl.gamepad != nullptr &&
                SDL_GetGamepadID(sdl.gamepad) == event.gdevice.which) {
                SDL_CloseGamepad(sdl.gamepad);
                sdl.gamepad = nullptr;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (configuring &&
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

void load_rom(const std::string& path,
              std::unique_ptr<gameboy::Emulator>& emulator) {
    auto replacement = std::make_unique<gameboy::Emulator>(
        gameboy::Cartridge::from_file(std::filesystem::u8path(path)));
    if (emulator) emulator->flush_battery();
    emulator = std::move(replacement);
}

void present(const gameboy::Emulator* emulator, SdlResources& sdl,
             const gameboy::DisplayPalette& palette) {
    if (!SDL_RenderClear(sdl.renderer)) {
        sdl_error("Could not clear framebuffer");
    }
    if (emulator != nullptr) {
        const auto& pixels = emulator->framebuffer();
        gameboy::Ppu::Framebuffer colored_pixels{};
        std::transform(pixels.begin(), pixels.end(), colored_pixels.begin(),
                       [&palette](const std::uint32_t pixel) {
                           return gameboy::apply_display_palette(pixel, palette);
                       });
        if (!SDL_UpdateTexture(sdl.texture, nullptr, colored_pixels.data(),
                               static_cast<int>(gameboy::Ppu::screen_width *
                                                sizeof(std::uint32_t))) ||
            !SDL_RenderTexture(sdl.renderer, sdl.texture, nullptr, nullptr)) {
            sdl_error("Could not present framebuffer");
        }
    }
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

int main(int argc, char** argv) {
    try {
        DialogState dialog;
        SdlResources sdl;
        const auto preference_path = preference_directory();
        auto recent_roms = load_recent_roms(preference_path);
        auto bindings = load_bindings(preference_path);
        auto configuration_backup = bindings;
        auto display_palette = load_display_palette(preference_path);
        std::unique_ptr<gameboy::Emulator> emulator;
        std::string current_rom;
        std::optional<std::string> pending_rom;
        std::optional<BindingConfiguration> configuring;
        auto paused = false;
        auto fullscreen = false;
        auto reset_requested = false;
        auto running = true;

        if (argc == 2) {
            pending_rom = argv[1];
        } else {
            if (argc > 2) {
                show_error(sdl.window, "Only one ROM can be opened at a time.");
            }
            show_rom_dialog(dialog, sdl.window);
        }

        using Clock = std::chrono::steady_clock;
        constexpr auto cycles_per_frame = 70224U;
        const auto frame_duration = std::chrono::duration<double>(
            static_cast<double>(cycles_per_frame) / 4194304.0);
        auto next_frame = Clock::now();

        while (running) {
            process_events(emulator, sdl, dialog, preference_path, bindings,
                           configuration_backup, recent_roms, current_rom,
                           configuring, pending_rom, display_palette, paused,
                           fullscreen, reset_requested, running);

            std::optional<std::string> dialog_error;
            collect_dialog_result(dialog, pending_rom, dialog_error);
            if (dialog_error) show_error(sdl.window, *dialog_error);

            if (reset_requested) {
                if (!current_rom.empty()) pending_rom = current_rom;
                reset_requested = false;
            }

            if (pending_rom) {
                try {
                    const bool reopening_current =
                        emulator && *pending_rom == current_rom;
                    load_rom(*pending_rom, emulator);
                    if (sdl.audio_stream != nullptr) {
                        static_cast<void>(SDL_ClearAudioStream(sdl.audio_stream));
                    }
                    current_rom = *pending_rom;
                    if (!reopening_current) paused = false;
                    remember_rom(current_rom, recent_roms, preference_path);
                    update_window_title(sdl.window, current_rom, paused,
                                        configuring);
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
                pending_rom.reset();
                next_frame = Clock::now();
            }

            if (emulator && !paused && !configuring &&
                !dialog_active(dialog)) {
                unsigned cycles = 0;
                while (running && cycles < cycles_per_frame &&
                       !emulator->frame_ready()) {
                    cycles += emulator->step();
                }
                if (emulator->frame_ready()) emulator->consume_frame();
            }
            submit_audio(emulator.get(), sdl);
            present(emulator.get(), sdl,
                    gameboy::display_palettes[display_palette]);

            next_frame += std::chrono::duration_cast<Clock::duration>(frame_duration);
            const auto now = Clock::now();
            if (next_frame > now) {
                std::this_thread::sleep_until(next_frame);
            } else if (now - next_frame > std::chrono::milliseconds(100)) {
                next_frame = now;
            }
        }
        if (emulator) emulator->flush_battery();
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
