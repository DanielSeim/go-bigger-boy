#include "dialogs.hpp"

#include "core_capability.hpp"
#include "emulation_session.hpp"
#include "settings_persistence.hpp"
#include "tool_window_support.hpp"
#include "window_event.hpp"

#include "gameboy/display_palette.hpp"
#include "gbb/frontend_logging.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <functional>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <vector>

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

#ifndef __ANDROID__
struct DesktopNotification {
    std::string message;
    std::uint64_t expires_at{};
    bool warning{};
};

struct DesktopDialog {
    std::string title;
    std::string message;
    std::vector<std::string> choices;
    std::size_t selected{};
    std::size_t scroll{};
    std::function<void(std::size_t)> on_choice;
};

std::unordered_map<SDL_Window*, DesktopDialog>& desktop_dialogs() {
    static std::unordered_map<SDL_Window*, DesktopDialog> dialogs;
    return dialogs;
}

std::unordered_map<SDL_Window*, DesktopNotification>&
desktop_notifications() {
    static std::unordered_map<SDL_Window*, DesktopNotification> notifications;
    return notifications;
}

void open_desktop_text_dialog(SDL_Window* window, std::string title,
                              std::string message) {
    if (window == nullptr) return;
    desktop_dialogs()[window] = {
        std::move(title), std::move(message), {}, 0, 0, {}};
}

void open_desktop_choice_dialog(
    SDL_Window* window, std::string title, std::string message,
    std::vector<std::string> choices, const std::size_t selected,
    std::function<void(std::size_t)> on_choice) {
    if (window == nullptr || choices.empty()) return;
    const auto choice_count = choices.size();
    desktop_dialogs()[window] = {
        std::move(title), std::move(message), std::move(choices),
        std::min(selected, choice_count - 1), 0, std::move(on_choice)};
}

void open_desktop_controls_dialog_impl(
    SDL_Window* window, const InputBindings& bindings,
    std::function<void(ControlsAction)> on_choice) {
    std::ostringstream message;
    message << "Choose a device to configure. Current keyboard bindings:\n\n";
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        message << button_names[index] << ": "
                << keyboard_key_setting_name(bindings.keys[index][0]);
        if (bindings.keys[index][1] != SDLK_UNKNOWN) {
            message << " / "
                    << keyboard_key_setting_name(bindings.keys[index][1]);
        }
        message << '\n';
    }
    open_desktop_choice_dialog(
        window, "Configure controls", message.str(),
        {"Keyboard", "Gamepad", "Restore defaults"}, 0,
        [on_choice = std::move(on_choice)](const std::size_t index) {
            if (!on_choice) return;
            on_choice(index == 0   ? ControlsAction::keyboard
                      : index == 1 ? ControlsAction::gamepad
                                   : ControlsAction::reset);
        });
}

struct DialogGeometry {
    float x{};
    float y{};
    float width{};
    float height{};
};

DialogGeometry dialog_geometry(SDL_Window* window) {
    int width = 0;
    int height = 0;
    static_cast<void>(SDL_GetWindowSize(window, &width, &height));
    const auto panel_width = std::min(860.0F,
                                      std::max(320.0F, width - 48.0F));
    const auto panel_height = std::min(680.0F,
                                       std::max(260.0F, height - 48.0F));
    return {(static_cast<float>(width) - panel_width) * 0.5F,
            (static_cast<float>(height) - panel_height) * 0.5F,
            panel_width, panel_height};
}

std::vector<std::string> wrap_dialog_message(const std::string& message,
                                             const std::size_t maximum) {
    std::vector<std::string> lines;
    std::istringstream input(message);
    std::string source;
    while (std::getline(input, source)) {
        if (source.empty()) {
            lines.emplace_back();
            continue;
        }
        while (source.size() > maximum) {
            auto split = source.rfind(' ', maximum);
            if (split == std::string::npos || split == 0) split = maximum;
            lines.push_back(source.substr(0, split));
            source.erase(0, split);
            while (!source.empty() && source.front() == ' ') source.erase(0, 1);
        }
        lines.push_back(std::move(source));
    }
    return lines;
}

SDL_FRect dialog_choice_rect(const DialogGeometry& geometry,
                             const std::size_t index,
                             const std::size_t count) {
    constexpr float gap = 10.0F;
    const auto width = (geometry.width - 48.0F -
                        gap * static_cast<float>(count - 1)) /
                       static_cast<float>(count);
    return {geometry.x + 24.0F +
                static_cast<float>(index) * (width + gap),
            geometry.y + geometry.height - 62.0F, width, 38.0F};
}

SDL_FRect dialog_close_rect(const DialogGeometry& geometry) {
    return {geometry.x + geometry.width - 126.0F,
            geometry.y + geometry.height - 56.0F, 102.0F, 34.0F};
}

SDL_FRect notification_rect(SDL_Window* window, const std::string& message) {
    int width = 0;
    int height = 0;
    static_cast<void>(SDL_GetWindowSize(window, &width, &height));
    const auto panel_width = std::min(680.0F,
                                      std::max(320.0F, width - 48.0F));
    const auto lines = wrap_dialog_message(
        message,
        static_cast<std::size_t>(std::max(24.0F, (panel_width - 48.0F) / 8.0F)));
    const auto visible_lines = std::min<std::size_t>(4, lines.size());
    const auto panel_height = std::min(
        180.0F, 74.0F + static_cast<float>(visible_lines) * 20.0F);
    return {(static_cast<float>(width) - panel_width) * 0.5F,
            static_cast<float>(height) - panel_height - 28.0F,
            panel_width, panel_height};
}

#endif

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
#ifndef __ANDROID__
    const auto current = std::distance(
        gameboy::video_modes.begin(),
        std::find_if(gameboy::video_modes.begin(), gameboy::video_modes.end(),
                     [mode = sdl.video_mode](const auto& info) {
                         return info.mode == mode;
                     }));
    std::vector<std::string> choices;
    choices.reserve(gameboy::video_modes.size());
    for (const auto& info : gameboy::video_modes) {
        choices.emplace_back(info.name);
    }
    open_desktop_choice_dialog(
        sdl.window, "Video pipeline",
        "Choose how Go Bigger Boy presents the Game Boy framebuffer.",
        std::move(choices), static_cast<std::size_t>(std::max<std::ptrdiff_t>(
                                    0, current)),
        [&sdl, preference_path](const std::size_t index) {
            if (index >= gameboy::video_modes.size()) return;
            if (!configure_video_pipeline(sdl, gameboy::video_modes[index].mode)) {
                show_error(sdl.window,
                           "Could not configure the selected video pipeline.");
                return;
            }
            save_video_mode(preference_path, gameboy::video_modes[index].mode);
        });
#else
    const auto selected = show_video_dialog(sdl.window, sdl.video_mode);
    if (!selected) return;
    if (!configure_video_pipeline(sdl, *selected)) {
        show_error(sdl.window, "Could not configure the selected video pipeline.");
        return;
    }
    save_video_mode(preference_path, *selected);
#endif
}

void choose_display_palette(gbb::EmulatorCore* core, SdlResources& sdl,
                            const std::filesystem::path& preference_path,
                            std::size_t& display_palette) {
    if (core != nullptr) release_all_buttons(*core);
#ifndef __ANDROID__
    std::vector<std::string> choices;
    choices.reserve(gameboy::display_palettes.size());
    for (const auto& palette : gameboy::display_palettes) {
        choices.emplace_back(palette.name);
    }
    open_desktop_choice_dialog(
        sdl.window, "Display palette",
        "Choose the display palette used for compatible Game Boy frames.",
        std::move(choices), display_palette,
        [core, &sdl, preference_path, &display_palette](const std::size_t index) {
            if (index >= gameboy::display_palettes.size()) return;
            display_palette = index;
            if (core != nullptr &&
                supports(core, gbb::CoreCapability::compatibility_palette)) {
                core->set_compatibility_colors(
                    gameboy::display_palettes[display_palette].cgb_compatibility);
            }
            save_display_palette(preference_path, display_palette);
        });
#else
    const auto selected = show_palette_dialog(sdl.window, display_palette);
    if (!selected) return;
    display_palette = *selected;
    if (core != nullptr &&
        supports(core, gbb::CoreCapability::compatibility_palette)) {
        core->set_compatibility_colors(
            gameboy::display_palettes[display_palette].cgb_compatibility);
    }
    save_display_palette(preference_path, display_palette);
#endif
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
        "Ctrl+Shift+H: Host a configured TCP or Bluetooth link\n"
        "Ctrl+Shift+J: Join a configured TCP or Bluetooth link\n"
        "Ctrl+Shift+D: Search the LAN for matching link hosts\n"
        "Ctrl+Shift+X: Stop the active remote link\n"
        "Player 2 (local link): W/A/S/D = directions, J/K = A/B, Q/E = Select/Start\n"
        "Game library: Choose the video pipeline\n"
        "Voxel mode: Drag vertically for pitch; horizontally for center-axis yaw\n"
        "Ctrl+1 through Ctrl+9: Open recent ROM\n"
        "Configured SaveState key: Save state\n"
        "Configured LoadState key: Load state\n"
        "Configured FastForward key: Hold for up to 4x speed\n"
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
        "Up/Down/Home/Ctrl+End: Navigate frames; Page Up/Down: Jump pages\n"
        "Insert/Delete/End: Edit timeline; drag cells to paint\n"
        "Ctrl+Z/Y: Undo/redo; Ctrl+C/V/D: Copy/paste/duplicate\n"
        "Backspace: Clear selected frame\n"
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
        "Shortcut notes: F11 toggles fullscreen in the game window but steps "
        "one frame in the debugger. F12 opens the debugger in the game window "
        "and closes it inside the debugger.\n\n"
        "Game Boy Printer pages are saved automatically as BMP images.\n"
        "Game Boy Camera cartridges use the first available webcam.\n"
        "Rumble cartridges vibrate the connected gamepad when supported.";
    const auto text = message.str();
#ifndef __ANDROID__
    open_desktop_text_dialog(window, "Go Bigger Boy controls", text);
#else
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_INFORMATION, "Go Bigger Boy (GBB) controls",
        text.c_str(), window));
#endif
}

void show_about(SDL_Window* window) {
#ifndef __ANDROID__
    open_desktop_text_dialog(
        window, "About Go Bigger Boy",
        "Go Bigger Boy (GBB) v" GBB_VERSION
        "\n\nA portable Game Boy and Game Boy Color emulator.");
#elif defined(_WIN32)
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
#ifndef __ANDROID__
    open_desktop_text_dialog(window, "Go Bigger Boy — attention required",
                             message);
#else
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
#endif
}

void show_lan_hosts(SDL_Window* window,
                    const std::vector<gameboy::LanPeer>& peers) {
    std::ostringstream message;
    if (peers.empty()) {
        message << "No matching Go Bigger Boy hosts were found on the LAN.";
    } else {
        message << "Matching Go Bigger Boy hosts:\n\n";
        for (const auto& peer : peers) {
            message << peer.name << " — " << peer.address << ':' << peer.port
                    << '\n';
        }
        message << "\nThe first host is selected for the next Join command.";
    }
#ifndef __ANDROID__
    open_desktop_text_dialog(window, "LAN link discovery", message.str());
#else
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_INFORMATION, "LAN link discovery", message.str().c_str(),
        window));
#endif
}

#ifndef __ANDROID__
void show_desktop_notification(SDL_Window* window, std::string message,
                               const bool warning) {
    if (window == nullptr || message.empty()) return;
    // Warnings remain available until the user opens their details. A brief
    // toast is appropriate for successful actions, but silently expiring a
    // link or save failure makes recovery unnecessarily difficult.
    const auto expires_at = warning
                                ? (std::numeric_limits<std::uint64_t>::max)()
                                : SDL_GetTicks() + 6000;
    desktop_notifications()[window] = {std::move(message), expires_at, warning};
}

bool desktop_notification_visible(SDL_Window* window) noexcept {
    if (window == nullptr) return false;
    auto& notifications = desktop_notifications();
    const auto found = notifications.find(window);
    if (found == notifications.end()) return false;
    if (found->second.expires_at <= SDL_GetTicks()) {
        notifications.erase(found);
        return false;
    }
    return true;
}

void present_desktop_notification(SDL_Renderer* renderer, SDL_Window* window) {
    auto& notifications = desktop_notifications();
    const auto found = notifications.find(window);
    if (found == notifications.end() || renderer == nullptr) return;
    if (found->second.expires_at <= SDL_GetTicks()) {
        notifications.erase(found);
        return;
    }
    const auto panel = notification_rect(window, found->second.message);
    const auto panel_width = panel.w;
    const auto lines = wrap_dialog_message(
        found->second.message,
        static_cast<std::size_t>(std::max(24.0F, (panel_width - 48.0F) / 8.0F)));
    const auto visible_lines = std::min<std::size_t>(4, lines.size());
    static_cast<void>(SDL_SetRenderLogicalPresentation(
        renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED));
    static_cast<void>(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND));
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 4, 9, 15, 238));
    static_cast<void>(SDL_RenderFillRect(renderer, &panel));
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer, found->second.warning ? 248 : 69,
        found->second.warning ? 183 : 207,
        found->second.warning ? 88 : 238, 255));
    static_cast<void>(SDL_RenderRect(renderer, &panel));
    render_tool_text(renderer, panel.x + 18.0F, panel.y + 12.0F,
                     found->second.warning ? "CHECK" : "DONE");
    for (std::size_t index = 0; index < visible_lines; ++index) {
        const auto clipped = index + 1 == visible_lines && lines.size() > visible_lines;
        render_tool_text(renderer, panel.x + 18.0F,
                         panel.y + 34.0F + static_cast<float>(index) * 20.0F,
                         clipped ? "..." : lines[index].c_str());
    }
    const SDL_FRect details{panel.x + panel.w - 96.0F,
                            panel.y + panel.h - 34.0F, 78.0F, 24.0F};
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 20, 77, 101, 255));
    static_cast<void>(SDL_RenderFillRect(renderer, &details));
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 69, 207, 238, 255));
    static_cast<void>(SDL_RenderRect(renderer, &details));
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 177, 192, 208, 255));
    render_tool_text(renderer, details.x + 12.0F, details.y + 7.0F, "DETAILS");
    static_cast<void>(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE));
}

bool handle_desktop_notification_event(const SDL_Event& event) {
    SDL_Window* window = nullptr;
    for (const auto& [candidate, unused] : desktop_notifications()) {
        if (event_window_id(event) == SDL_GetWindowID(candidate)) {
            window = candidate;
            break;
        }
    }
    if (window == nullptr || event.type != SDL_EVENT_MOUSE_BUTTON_UP ||
        event.button.button != SDL_BUTTON_LEFT) {
        return false;
    }
    auto& notifications = desktop_notifications();
    const auto found = notifications.find(window);
    if (found == notifications.end()) return false;
    const auto panel = notification_rect(window, found->second.message);
    if (event.button.x < panel.x || event.button.x > panel.x + panel.w ||
        event.button.y < panel.y || event.button.y > panel.y + panel.h) {
        return false;
    }
    auto message = found->second.message;
    notifications.erase(found);
    open_desktop_text_dialog(window, "Notification details", std::move(message));
    return true;
}

bool desktop_dialog_visible(SDL_Window* window) noexcept {
    return window != nullptr && desktop_dialogs().find(window) !=
                                    desktop_dialogs().end();
}

bool handle_desktop_dialog_event(const SDL_Event& event) {
    SDL_Window* window = nullptr;
    for (const auto& [candidate, unused] : desktop_dialogs()) {
        if (event_window_id(event) == SDL_GetWindowID(candidate)) {
            window = candidate;
            break;
        }
    }
    if (window == nullptr) {
        if (event.type == SDL_EVENT_QUIT && !desktop_dialogs().empty()) {
            return false;
        }
        return !desktop_dialogs().empty();
    }
    auto found = desktop_dialogs().find(window);
    if (found == desktop_dialogs().end()) return false;
    auto& dialog = found->second;
    const auto close = [&] { desktop_dialogs().erase(found); };
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        if (event.key.key == SDLK_ESCAPE) {
            close();
        } else if (!dialog.choices.empty() && event.key.key == SDLK_LEFT &&
                   dialog.selected > 0) {
            --dialog.selected;
        } else if (!dialog.choices.empty() && event.key.key == SDLK_RIGHT &&
                   dialog.selected + 1 < dialog.choices.size()) {
            ++dialog.selected;
        } else if (event.key.key == SDLK_UP && dialog.scroll > 0) {
            --dialog.scroll;
        } else if (event.key.key == SDLK_DOWN && dialog.choices.empty()) {
            ++dialog.scroll;
        } else if (event.key.key == SDLK_PAGEUP && dialog.choices.empty()) {
            dialog.scroll = dialog.scroll > 12 ? dialog.scroll - 12 : 0;
        } else if (event.key.key == SDLK_PAGEDOWN && dialog.choices.empty()) {
            dialog.scroll += 12;
        } else if ((event.key.key == SDLK_RETURN ||
                    event.key.key == SDLK_KP_ENTER) && !dialog.choices.empty()) {
            const auto choice = dialog.selected;
            auto callback = std::move(dialog.on_choice);
            close();
            if (callback) callback(choice);
        }
        return true;
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL && dialog.choices.empty()) {
        if (event.wheel.y > 0) {
            dialog.scroll = dialog.scroll > 3 ? dialog.scroll - 3 : 0;
        } else if (event.wheel.y < 0) {
            dialog.scroll += 3;
        }
        return true;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION && !dialog.choices.empty()) {
        const auto geometry = dialog_geometry(window);
        for (std::size_t index = 0; index < dialog.choices.size(); ++index) {
            const auto rect = dialog_choice_rect(geometry, index,
                                                 dialog.choices.size());
            if (event.motion.x >= rect.x && event.motion.x <= rect.x + rect.w &&
                event.motion.y >= rect.y && event.motion.y <= rect.y + rect.h) {
                dialog.selected = index;
                break;
            }
        }
        return true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        event.button.button == SDL_BUTTON_LEFT) {
        const auto geometry = dialog_geometry(window);
        const auto x = event.button.x;
        const auto y = event.button.y;
        if (dialog.choices.empty()) {
            const auto close_rect = dialog_close_rect(geometry);
            if (x >= close_rect.x && x <= close_rect.x + close_rect.w &&
                y >= close_rect.y && y <= close_rect.y + close_rect.h) {
                close();
            }
            return true;
        }
        for (std::size_t index = 0; index < dialog.choices.size(); ++index) {
            const auto rect = dialog_choice_rect(geometry, index,
                                                 dialog.choices.size());
            if (x >= rect.x && x <= rect.x + rect.w && y >= rect.y &&
                y <= rect.y + rect.h) {
                auto callback = std::move(dialog.on_choice);
                close();
                if (callback) callback(index);
                break;
            }
        }
        return true;
    }
    return true;
}

void present_desktop_dialog(SDL_Renderer* renderer, SDL_Window* window) {
    const auto found = desktop_dialogs().find(window);
    if (found == desktop_dialogs().end() || renderer == nullptr) return;
    const auto& dialog = found->second;
    const auto geometry = dialog_geometry(window);
    static_cast<void>(SDL_SetRenderLogicalPresentation(
        renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED));
    static_cast<void>(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND));
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 190));
    int width = 0;
    int height = 0;
    static_cast<void>(SDL_GetWindowSize(window, &width, &height));
    const SDL_FRect veil{0, 0, static_cast<float>(width),
                         static_cast<float>(height)};
    static_cast<void>(SDL_RenderFillRect(renderer, &veil));
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 12, 22, 32, 255));
    const SDL_FRect panel{geometry.x, geometry.y, geometry.width, geometry.height};
    static_cast<void>(SDL_RenderFillRect(renderer, &panel));
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 69, 207, 238, 255));
    static_cast<void>(SDL_RenderRect(renderer, &panel));
    const auto content_width = geometry.width - 48.0F;
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 230, 249, 255, 255));
    render_tool_text(renderer, geometry.x + 24, geometry.y + 20,
                     dialog.title.c_str(), content_width);
    const auto maximum = static_cast<std::size_t>(std::max(
        24.0F, (geometry.width - 48.0F) / 8.0F));
    const auto lines = wrap_dialog_message(dialog.message, maximum);
    const auto content_top = geometry.y + 58.0F;
    const auto content_bottom = dialog.choices.empty()
                                    ? geometry.y + geometry.height - 96.0F
                                    : geometry.y + geometry.height - 82.0F;
    const auto visible_lines = static_cast<std::size_t>(std::max(
        1.0F, (content_bottom - content_top) / 20.0F));
    const auto first = std::min(dialog.scroll,
                                lines.size() > visible_lines
                                    ? lines.size() - visible_lines
                                    : std::size_t{0});
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 177, 192, 208, 255));
    for (std::size_t index = 0; index < visible_lines && first + index < lines.size();
         ++index) {
        render_tool_text(renderer, geometry.x + 24,
                         content_top + static_cast<float>(index) * 20.0F,
                         lines[first + index].c_str(), content_width);
    }
    if (!dialog.choices.empty()) {
        for (std::size_t index = 0; index < dialog.choices.size(); ++index) {
            const auto rect = dialog_choice_rect(geometry, index,
                                                 dialog.choices.size());
            static_cast<void>(SDL_SetRenderDrawColor(
                renderer, index == dialog.selected ? 20 : 28,
                index == dialog.selected ? 104 : 47,
                index == dialog.selected ? 135 : 68, 255));
            static_cast<void>(SDL_RenderFillRect(renderer, &rect));
            static_cast<void>(SDL_SetRenderDrawColor(
                renderer, index == dialog.selected ? 69 : 112,
                index == dialog.selected ? 207 : 160,
                index == dialog.selected ? 238 : 183, 255));
            static_cast<void>(SDL_RenderRect(renderer, &rect));
            render_tool_text(renderer, rect.x + 10, rect.y + 10,
                             dialog.choices[index].c_str(), rect.w - 20.0F);
        }
    } else {
        const auto close = dialog_close_rect(geometry);
        static_cast<void>(SDL_SetRenderDrawColor(renderer, 20, 77, 101, 255));
        static_cast<void>(SDL_RenderFillRect(renderer, &close));
        static_cast<void>(SDL_SetRenderDrawColor(renderer, 69, 207, 238, 255));
        static_cast<void>(SDL_RenderRect(renderer, &close));
        static_cast<void>(SDL_SetRenderDrawColor(renderer, 177, 192, 208, 255));
        render_tool_text(renderer, close.x + 28, close.y + 10, "CLOSE",
                         close.w - 56.0F);
    }
    static_cast<void>(SDL_SetRenderDrawColor(renderer, 137, 160, 183, 255));
    const auto footer = dialog.choices.empty()
                            ? "UP/DOWN SCROLL  PAGE UP/DOWN  ESC CLOSE"
                            : "LEFT/RIGHT CHOOSE  ENTER SELECT  ESC CANCEL";
    render_tool_text(renderer, geometry.x + 24, geometry.y + geometry.height - 30,
                     footer, content_width);
    static_cast<void>(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE));
}
#endif

#ifndef __ANDROID__
void open_desktop_controls_dialog(
    SDL_Window* window, const InputBindings& bindings,
    std::function<void(ControlsAction)> on_choice) {
    open_desktop_controls_dialog_impl(window, bindings, std::move(on_choice));
}
#endif

} // namespace gbb::sdl
