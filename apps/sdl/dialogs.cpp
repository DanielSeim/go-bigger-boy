#include "dialogs.hpp"

#include "core_capability.hpp"
#include "emulation_session.hpp"
#include "settings_persistence.hpp"

#include "gameboy/display_palette.hpp"
#include "gbb/frontend_logging.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "resource.h"
#endif

#ifndef GBB_VERSION
#define GBB_VERSION "0.0.0-dev"
#endif

namespace gbb::sdl {
namespace {

[[nodiscard]] std::string rom_filename_for_title(const std::string& path) {
    auto name = std::filesystem::u8path(path).filename().u8string();
#ifdef __ANDROID__
    if (name.size() > 17 && name[16] == '-' &&
        std::all_of(name.begin(), name.begin() + 16, [](const char c) {
            return std::isxdigit(static_cast<unsigned char>(c)) != 0;
        })) {
        name.erase(0, 17);
    }
#endif
    return name.empty() ? path : name;
}

[[nodiscard]] std::optional<std::size_t>
show_palette_dialog(SDL_Window* window, const std::size_t current) {
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

[[nodiscard]] std::optional<gameboy::VideoMode>
show_video_dialog(SDL_Window* window, const gameboy::VideoMode current) {
    std::array<SDL_MessageBoxButtonData, gameboy::video_modes.size() + 1>
        buttons{};
    for (std::size_t index = 0; index < gameboy::video_modes.size(); ++index) {
        buttons[index] = {0, static_cast<int>(index),
                          gameboy::video_modes[index].name.data()};
    }
    buttons.back() = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, -1, "Cancel"};
    const auto message = std::string("Current pipeline: ") +
                         std::string{gameboy::video_mode_info(current).name} +
                         "\n\nChoose a presentation mode:";
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_INFORMATION, window, "Video pipeline", message.c_str(),
        static_cast<int>(buttons.size()), buttons.data(), nullptr,
    };
    auto selection = -1;
    if (!SDL_ShowMessageBox(&box, &selection) || selection < 0 ||
        static_cast<std::size_t>(selection) >= gameboy::video_modes.size()) {
        return std::nullopt;
    }
    return gameboy::video_modes[static_cast<std::size_t>(selection)].mode;
}

} // namespace

void update_window_title(
    SDL_Window* window, const std::string& current_rom, const bool paused,
    const std::optional<BindingConfiguration>& configuring) {
    std::string title = "Go Bigger Boy (GBB)";
    if (configuring) {
        title += configuring->device == BindingDevice::keyboard
                     ? " - Press a key for "
                     : " - Press a gamepad button for ";
        title += button_names[configuring->index];
        if (configuring->device == BindingDevice::keyboard) {
            title += configuring->slot == 0 ? " (primary)"
                                            : " (secondary; Space: none)";
        }
        title += " (Esc: cancel)";
    } else {
        if (!current_rom.empty()) {
            title += " - " + rom_filename_for_title(current_rom);
        } else {
            title += " - Drop a ROM here or press Ctrl+O";
        }
        if (paused) title += " [PAUSED]";
        title += "  (F1: Help)";
    }
    if (!SDL_SetWindowTitle(window, title.c_str())) {
        throw std::runtime_error("Could not update window title: " +
                                 std::string(SDL_GetError()));
    }
}

void choose_video_mode(SdlResources& sdl,
                       const std::filesystem::path& preference_path) {
    const auto selected = show_video_dialog(sdl.window, sdl.video_mode);
    if (!selected) return;
    if (!configure_video_pipeline(sdl, *selected)) {
        show_error(sdl.window, "Could not configure the selected video pipeline.");
        return;
    }
    save_video_mode(preference_path, *selected);
}

void choose_display_palette(gbb::EmulatorCore* core, SdlResources& sdl,
                            const std::filesystem::path& preference_path,
                            std::size_t& display_palette) {
    if (core != nullptr) release_all_buttons(*core);
    const auto selected = show_palette_dialog(sdl.window, display_palette);
    if (!selected) return;
    display_palette = *selected;
    if (core != nullptr &&
        supports(core, gbb::CoreCapability::compatibility_palette)) {
        core->set_compatibility_colors(
            gameboy::display_palettes[display_palette].cgb_compatibility);
    }
    save_display_palette(preference_path, display_palette);
}

bool confirm_exit(SDL_Window* window) {
    constexpr std::array<SDL_MessageBoxButtonData, 2> buttons{{
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Exit"},
    }};
    constexpr auto message = "Are you sure you want to close Go Bigger Boy?";
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_WARNING, window, "Exit Go Bigger Boy?", message,
        static_cast<int>(buttons.size()), buttons.data(), nullptr,
    };
    auto selection = 0;
    return SDL_ShowMessageBox(&box, &selection) && selection == 1;
}

void show_help(SDL_Window* window, const InputBindings& bindings) {
    std::ostringstream message;
    message << "Version " GBB_VERSION "\n\nGAMEPLAY CONTROLS\n";
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        message << button_names[index] << ": "
                << keyboard_key_setting_name(bindings.keys[index][0]);
        if (bindings.keys[index][1] != SDLK_UNKNOWN) {
            message << " / "
                    << keyboard_key_setting_name(bindings.keys[index][1]);
        }
        message << '\n';
    }
    message << "\nCONFIGURABLE EMULATOR SHORTCUTS\n";
    for (std::size_t index = 0; index < shortcut_names.size(); ++index) {
        message << shortcut_names[index] << ": "
                << keyboard_key_setting_name(bindings.shortcuts[index]) << '\n';
    }
    message <<
        "\nGENERAL\n"
        "Space: Pause/resume\n"
        "Ctrl+R: Reset\n"
        "Ctrl+O: Open ROM\n"
        "Ctrl+L: Open game library\n"
        "Ctrl+K: Configure controls\n"
        "Ctrl+P: Choose display palette\n"
        "Ctrl+G: Open GameShark cheat manager\n"
        "Ctrl+Shift+L: Start/stop a local two-player link\n"
        "Ctrl+Shift+R: Retry a stalled link handshake\n"
        "Ctrl+Shift+H: Host a TCP link on 127.0.0.1:8765\n"
        "Ctrl+Shift+J: Join a TCP link on 127.0.0.1:8765\n"
        "Ctrl+Shift+X: Stop the active TCP link\n"
        "Player 2 (local link): W/A/S/D = directions, J/K = A/B, Q/E = Select/Start\n"
        "Game library: Choose the video pipeline\n"
        "Voxel mode: Drag vertically for pitch; horizontally for center-axis yaw\n"
        "Ctrl+1 through Ctrl+9: Open recent ROM\n"
        "Configured SaveState key: Save state\n"
        "Configured LoadState key: Load state\n"
        "Configured FastForward key: Hold for 4x speed\n"
        "Configured Rewind key: Hold to rewind\n"
        "F12: Open/close debugger\n"
        "Debugger F5: Run/pause\n"
        "Debugger F6: Start/stop input recording\n"
        "Debugger F7: Replay the latest recording\n"
        "Debugger F8: Open the TAS frame editor\n"
        "Debugger F9: Open the live sprite editor\n"
        "Debugger F10: Step one instruction\n"
        "Debugger F11: Step one frame\n"
        "\nTAS EDITOR\n"
        "Up/Down: Select frame; Insert/Delete/End: Edit timeline\n"
        "Ctrl+N: New from current state; Ctrl+S: Save; F7: Run\n"
        "\nSPRITE EDITOR\n"
        "1-4: Color; Left/right mouse: Paint/erase\n"
        "Ctrl+Z: Undo; Delete: Clear; B: Switch CGB bank\n"
        "Ctrl+S: Save tile patch; Ctrl+O: Import; Ctrl+E: Export IPS\n"
        "\nGAMESHARK CHEAT MANAGER\n"
        "Ctrl+G: Open for the current ROM\n"
        "Space: Toggle selected cheat; Delete: Remove selected cheat\n"
        "Fetch for ROM: Import matching Libretro archive entries\n"
        "F11: Toggle fullscreen\n"
        "F1: Show this help\n"
        "Escape: Quit\n\n"
        "Game Boy Printer pages are saved automatically as BMP images.\n"
        "Game Boy Camera cartridges use the first available webcam.\n"
        "Rumble cartridges vibrate the connected gamepad when supported.";
    const auto text = message.str();
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_INFORMATION, "Go Bigger Boy (GBB) controls",
        text.c_str(), window));
}

void show_about(SDL_Window* window) {
#ifdef _WIN32
    const auto owner = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr));
    std::wstring message = L"Go Bigger Boy (GBB) v";
    for (const auto character : std::string_view{GBB_VERSION}) {
        message.push_back(static_cast<wchar_t>(
            static_cast<unsigned char>(character)));
    }
    message += L"\n\nA portable Game Boy and Game Boy Color emulator.";
    MSGBOXPARAMSW parameters{};
    parameters.cbSize = sizeof(parameters);
    parameters.hwndOwner = owner;
    parameters.hInstance = GetModuleHandleW(nullptr);
    parameters.lpszText = message.c_str();
    parameters.lpszCaption = L"About Go Bigger Boy";
    parameters.dwStyle = MB_OK | MB_USERICON | MB_SETFOREGROUND;
    parameters.lpszIcon = MAKEINTRESOURCEW(IDI_GBB_ICON);
    static_cast<void>(MessageBoxIndirectW(&parameters));
#else
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_INFORMATION, "About Go Bigger Boy",
        "Go Bigger Boy (GBB) v" GBB_VERSION
        "\n\nA portable Game Boy and Game Boy Color emulator.", window));
#endif
}

void show_error(SDL_Window* window, const std::string& message) {
    gbb::log_frontend_error(message);
#ifdef _WIN32
    HWND owner = nullptr;
    if (window != nullptr) {
        owner = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
            nullptr));
    }
    MessageBoxA(owner, message.c_str(), "Go Bigger Boy (GBB)",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#else
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR, "Go Bigger Boy (GBB)", message.c_str(), window));
#endif
}

} // namespace gbb::sdl
