#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "windows_dashboard.hpp"
#include "windows_dashboard_artwork.hpp"
#include "windows_dashboard_smoke.hpp"
#include "windows_dashboard_state.hpp"
#include "resource.h"
#include "update_checker.hpp"

#ifdef _WIN32

#include <SDL3/SDL.h>

#include <commctrl.h>
#include <commdlg.h>
#include <bluetoothapis.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace gbb_desktop {
namespace {

#ifndef GBB_VERSION
#define GBB_VERSION "0.0.0-dev"
#endif

constexpr int id_library = 100;
constexpr int id_settings = 101;
constexpr int id_list = 102;
constexpr int id_open = 103;
constexpr int id_play = 104;
constexpr int id_resume = 105;
constexpr int id_search = 106;
constexpr int id_palette = 107;
constexpr int id_remove = 108;
constexpr int id_video = 109;
constexpr int id_hardware_model = 130;
constexpr int id_audio_enabled = 131;
constexpr int id_gameboy_background = 110;
constexpr int id_reset_controls = 111;
constexpr int id_shortcuts = 112;
constexpr int id_voxel_save = 113;
constexpr int id_voxel_reset = 114;
constexpr int id_voxel_preview = 115;
constexpr int id_plugin_discovery = 116;
constexpr int id_plugin_require_allowlist = 117;
constexpr int id_plugin_require_capability_allowlist = 118;
constexpr int id_voxel_first_edit = 120;
constexpr int id_link_transport = 121;
constexpr int id_link_remote_host = 122;
constexpr int id_link_remote_bind = 123;
constexpr int id_link_remote_port = 124;
constexpr int id_link_lan_discovery = 125;
constexpr int id_link_bluetooth_address = 126;
constexpr int id_link_bluetooth_uuid = 127;
constexpr int id_link_diagnostics = 128;
constexpr int id_link_bluetooth_choose = 135;
constexpr int id_settings_apply = 129;
constexpr int id_settings_cancel = 132;
constexpr int id_search_clear = 133;
constexpr int id_artwork_retry = 134;
constexpr int id_settings_section_first = 136;
constexpr int id_binding_first = 200;
constexpr int id_action_first = 220;
constexpr UINT artwork_ready = WM_APP + 1;
constexpr UINT update_poll_timer = 2;
constexpr int dashboard_width = 980;
// Keep the initial dashboard usable on 1080p displays after non-client
// chrome, while the settings page remains fully accessible through scrolling.
constexpr int dashboard_height = 900;
// Navigation, section tabs, and the description remain fixed while the
// selected settings page scrolls below them.
constexpr int settings_content_top = 320;
constexpr auto dashboard_content_clip_property =
    L"GBB_DASHBOARD_CONTENT_CLIPPED";

HBITMAP load_file_bitmap(const std::filesystem::path& path, UINT width,
                         UINT height);

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                            static_cast<int>(value.size()),
                                            nullptr, 0);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                            static_cast<int>(value.size()),
                                            nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), count,
                        nullptr, nullptr);
    return result;
}

std::wstring plugin_status_text(const gbb::PluginDiscoveryOptions& options,
                               const gbb::PluginCatalog& catalog) {
    std::size_t rejected = 0;
    for (const auto& diagnostic : catalog.diagnostics()) {
        if (!diagnostic.loaded) ++rejected;
    }
    std::wostringstream text;
    text << L"Loaded: " << catalog.loaded_count()
         << L"    Rejected: " << rejected << L"\r\n"
         << (options.enabled ? L"Discovery enabled" : L"Discovery disabled")
         << (options.require_allowlist ? L"; identity allowlist required"
                                       : L"; identity allowlist optional")
         << (options.require_capability_allowlist
                 ? L"; capability allowlist required"
                 : L"; capability allowlist optional")
         << L"\r\nPaths, allowed core IDs, and capability IDs are configured in settings.ini.";
    if (rejected != 0) {
        text << L"\r\n\r\nFirst rejection:";
        for (const auto& diagnostic : catalog.diagnostics()) {
            if (diagnostic.loaded) continue;
            text << L"\r\n- " << widen(diagnostic.message);
            break;
        }
    }
    return text.str();
}

std::wstring formatted_last_played(const std::int64_t timestamp) {
    if (timestamp <= 0) return L"Unknown";
    const auto value = static_cast<std::time_t>(timestamp);
    std::tm local{};
    if (localtime_s(&local, &value) != 0) return L"Unknown";
    std::array<wchar_t, 64> result{};
    return std::wcsftime(result.data(), result.size(), L"%Y-%m-%d %H:%M",
                         &local) == 0
               ? std::wstring{L"Unknown"}
               : std::wstring{result.data()};
}

using State = DashboardState;

long settings_content_bottom(const State& state) {
    switch (state.settings_section) {
    case State::SettingsSection::general:
        return 548;
    case State::SettingsSection::controls:
        return 854;
    case State::SettingsSection::link:
        return 572;
    case State::SettingsSection::advanced:
        // The plug-in controls are the last visible controls in this
        // section. Voxel controls move them down when a ROM is active.
        return (state.voxel_available ? 620L : 350L) + 287L;
    }
    return 548;
}

std::optional<POINT> load_window_position(
    const std::filesystem::path& preference_directory) {
    if (preference_directory.empty()) return std::nullopt;
    std::ifstream input(preference_directory / "dashboard-window.txt");
    POINT position{};
    if (!(input >> position.x >> position.y)) return std::nullopt;
    RECT rectangle{position.x, position.y,
                   position.x + dashboard_width,
                   position.y + dashboard_height};
    const auto monitor = MonitorFromRect(&rectangle, MONITOR_DEFAULTTONULL);
    if (monitor == nullptr) return std::nullopt;
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return std::nullopt;
    // Keep the complete dashboard on the saved monitor. Older versions only
    // checked for a tiny intersection, which could strand settings controls
    // off-screen after a monitor or DPI change.
    const auto& work = info.rcWork;
    position.x = std::clamp(position.x, work.left,
                            std::max(work.left, work.right - dashboard_width));
    position.y = std::clamp(position.y, work.top,
                            std::max(work.top, work.bottom - dashboard_height));
    return position;
}

void save_window_position(const State& state) {
    if (state.preference_directory.empty() || state.window == nullptr ||
        IsIconic(state.window)) {
        return;
    }
    RECT rectangle{};
    if (!GetWindowRect(state.window, &rectangle)) return;
    std::ofstream output(state.preference_directory / "dashboard-window.txt",
                         std::ios::trunc);
    output << rectangle.left << ' ' << rectangle.top << '\n';
}

constexpr std::array<const wchar_t*, 8> control_names{{
    L"Right", L"Left", L"Up", L"Down", L"A", L"B", L"Select", L"Start"}};
constexpr std::array<const wchar_t*, 4> action_names{{
    L"Fast Forward", L"Rewind", L"Save State", L"Load State"}};

KeyboardBindings default_keyboard_bindings() {
    return {{{{SDLK_RIGHT, SDLK_UNKNOWN}}, {{SDLK_LEFT, SDLK_UNKNOWN}},
             {{SDLK_UP, SDLK_UNKNOWN}}, {{SDLK_DOWN, SDLK_UNKNOWN}},
             {{SDLK_X, SDLK_UNKNOWN}}, {{SDLK_Z, SDLK_UNKNOWN}},
             {{SDLK_BACKSPACE, SDLK_UNKNOWN}},
             {{SDLK_RETURN, SDLK_UNKNOWN}}}};
}

ActionBindings default_action_bindings() {
    return {{SDLK_TAB, SDLK_LSHIFT, SDLK_F5, SDLK_F8}};
}

std::wstring edit_value(const HWND control) {
    if (control == nullptr) return {};
    const auto length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

std::wstring bluetooth_address_text(const BLUETOOTH_ADDRESS& address) {
    std::wostringstream compact;
    compact << std::uppercase << std::hex << std::setfill(L'0')
            << std::setw(12) << address.ullLong;
    const auto value = compact.str();
    std::wstring formatted;
    formatted.reserve(17);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        if (index != 0) formatted.push_back(L':');
        formatted.append(value, index, 2);
    }
    return formatted;
}

bool choose_bluetooth_device(State& state) {
    // The Windows SDK exposes these functions through Bthprops.lib, but the
    // MinGW toolchain used for local Windows builds does not ship that import
    // library. Resolve the stable system DLL entry points at runtime so the
    // picker remains available on Windows without making the cross-build
    // depend on an SDK-specific import library.
    const auto module = LoadLibraryW(L"bthprops.cpl");
    if (module == nullptr) return false;
    using SelectDevices = BOOL(WINAPI *)(BLUETOOTH_SELECT_DEVICE_PARAMS*);
    using FreeDevices = BOOL(WINAPI *)(BLUETOOTH_SELECT_DEVICE_PARAMS*);
    const auto select_devices = reinterpret_cast<SelectDevices>(
        GetProcAddress(module, "BluetoothSelectDevices"));
    const auto free_devices = reinterpret_cast<FreeDevices>(
        GetProcAddress(module, "BluetoothSelectDevicesFree"));
    if (select_devices == nullptr || free_devices == nullptr) {
        FreeLibrary(module);
        return false;
    }

    BLUETOOTH_SELECT_DEVICE_PARAMS params{};
    params.dwSize = sizeof(params);
    params.hwndParent = state.window;
    params.fShowAuthenticated = TRUE;
    params.fShowRemembered = TRUE;
    params.fShowUnknown = FALSE;
    params.fAddNewDeviceWizard = FALSE;
    params.fSkipServicesPage = TRUE;
    params.cNumDevices = 1;
    if (!select_devices(&params)) {
        FreeLibrary(module);
        return false;
    }

    bool selected = params.pDevices != nullptr && params.cNumDevices > 0 &&
                    (params.pDevices[0].fAuthenticated ||
                     params.pDevices[0].fRemembered);
    if (selected) {
        SetWindowTextW(state.link_bluetooth_address,
                       bluetooth_address_text(params.pDevices[0].Address).c_str());
    }
    free_devices(&params);
    FreeLibrary(module);
    return selected;
}

bool equal_link_settings(const DashboardLinkSettings& left,
                         const DashboardLinkSettings& right) {
    return left.transport == right.transport &&
           left.remote_host == right.remote_host &&
           left.remote_bind == right.remote_bind &&
           left.remote_port == right.remote_port &&
           left.lan_discovery == right.lan_discovery &&
           left.bluetooth_address == right.bluetooth_address &&
           left.bluetooth_service_uuid == right.bluetooth_service_uuid &&
           left.diagnostics == right.diagnostics;
}

void mark_settings_dirty(State& state);

constexpr auto dashboard_checkbox_state_property =
    L"GBB_DASHBOARD_CHECKBOX_STATE";

bool dashboard_checkbox_checked(HWND checkbox) {
    const auto state = GetPropW(checkbox, dashboard_checkbox_state_property);
    if (state != nullptr) {
        return reinterpret_cast<INT_PTR>(state) == 2;
    }
    return SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void set_dashboard_checkbox_checked(HWND checkbox, const bool checked) {
    if (checkbox == nullptr) return;
    SetPropW(checkbox, dashboard_checkbox_state_property,
             reinterpret_cast<HANDLE>(static_cast<INT_PTR>(checked ? 2 : 1)));
    InvalidateRect(checkbox, nullptr, TRUE);
}

void toggle_dashboard_checkbox(HWND checkbox) {
    set_dashboard_checkbox_checked(checkbox,
                                   !dashboard_checkbox_checked(checkbox));
}

DashboardLinkSettings read_link_settings(State& state) {
    auto settings = state.initial_link_settings;
    const auto selected = SendMessageW(state.link_transport, CB_GETCURSEL, 0, 0);
    settings.transport = selected == 1 ? "bluetooth" : "tcp";

    const auto host = narrow(edit_value(state.link_remote_host));
    if (!host.empty()) settings.remote_host = host;
    const auto bind = narrow(edit_value(state.link_remote_bind));
    if (!bind.empty()) settings.remote_bind = bind;
    const auto address = narrow(edit_value(state.link_bluetooth_address));
    settings.bluetooth_address = address;
    const auto uuid = narrow(edit_value(state.link_bluetooth_uuid));
    if (!uuid.empty()) settings.bluetooth_service_uuid = uuid;

    const auto port_text = narrow(edit_value(state.link_remote_port));
    try {
        const auto parsed = std::stoul(port_text);
        if (parsed > 0 && parsed <= 65535) {
            settings.remote_port = static_cast<std::uint16_t>(parsed);
        }
    } catch (...) {
        // Preserve the last valid port until the user enters a valid value.
    }
    settings.lan_discovery = dashboard_checkbox_checked(
        state.link_lan_discovery);
    settings.diagnostics = dashboard_checkbox_checked(state.link_diagnostics);
    return settings;
}

bool link_port_valid(const State& state) {
    if (SendMessageW(state.link_transport, CB_GETCURSEL, 0, 0) == 1) {
        return true;
    }
    const auto text = narrow(edit_value(state.link_remote_port));
    if (text.empty()) return false;
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoul(text, &consumed);
        return consumed == text.size() && parsed > 0 && parsed <= 65535;
    } catch (...) {
        return false;
    }
}

void show_link_port_validation(State& state) {
    if (!link_port_valid(state)) {
        SetWindowTextW(state.settings_status,
                       L"TCP port must be a number from 1 to 65535.");
    } else {
        mark_settings_dirty(state);
    }
}

void collect_link_settings(State& state) {
    state.result.link_settings = read_link_settings(state);
    state.result.link_settings_changed = !equal_link_settings(
        state.result.link_settings, state.initial_link_settings);
}

void mark_settings_dirty(State& state) {
    state.settings_dirty = true;
    if (state.settings_status != nullptr) {
        SetWindowTextW(state.settings_status,
                       L"Unsaved changes. Apply to keep them, or discard them.");
    }
}

void update_link_control_state(State& state) {
    const auto link_section =
        state.page == State::Page::settings &&
        state.settings_section == State::SettingsSection::link;
    const auto bluetooth = SendMessageW(
        state.link_transport, CB_GETCURSEL, 0, 0) == 1;
    const auto set_enabled = [bluetooth](const HWND control, const bool bt) {
        if (control != nullptr) EnableWindow(control, bluetooth == bt);
    };
    // TCP endpoint fields are irrelevant for Bluetooth; hiding that distinction
    // is a common source of misconfigured sessions. Keep the values intact so
    // switching transport does not discard a user's previous endpoint.
    set_enabled(state.link_remote_host_label, false);
    set_enabled(state.link_remote_host, false);
    set_enabled(state.link_remote_bind_label, false);
    set_enabled(state.link_remote_bind, false);
    set_enabled(state.link_remote_port_label, false);
    set_enabled(state.link_remote_port, false);
    set_enabled(state.link_lan_discovery, false);
    set_enabled(state.link_bluetooth_address_label, true);
    set_enabled(state.link_bluetooth_address, true);
    set_enabled(state.link_bluetooth_choose, true);
    set_enabled(state.link_bluetooth_uuid_label, true);
    set_enabled(state.link_bluetooth_uuid, true);

    const auto set_visible = [link_section](const HWND control,
                                             const bool visible) {
        if (control != nullptr) {
            ShowWindow(control, link_section && visible ? SW_SHOW : SW_HIDE);
        }
    };
    set_visible(state.link_remote_host_label, !bluetooth);
    set_visible(state.link_remote_host, !bluetooth);
    set_visible(state.link_remote_bind_label, !bluetooth);
    set_visible(state.link_remote_bind, !bluetooth);
    set_visible(state.link_remote_port_label, !bluetooth);
    set_visible(state.link_remote_port, !bluetooth);
    set_visible(state.link_lan_discovery, !bluetooth);
    set_visible(state.link_bluetooth_address_label, bluetooth);
    set_visible(state.link_bluetooth_address, bluetooth);
    set_visible(state.link_bluetooth_choose, bluetooth);
    set_visible(state.link_bluetooth_uuid_label, bluetooth);
    set_visible(state.link_bluetooth_uuid, bluetooth);
    set_visible(state.link_diagnostics, true);
}

void show_settings_section(State& state) {
    const auto settings = state.page == State::Page::settings;
    const auto section = state.settings_section;
    constexpr std::array<const wchar_t*, 4> descriptions{{
        L"Display palette, video mode, audio generation, and hardware model. Hardware changes apply to the next game.",
        L"Click a slot, press a key, or press Delete to clear it. Duplicate keys are moved from their previous action.",
        L"Choose TCP for a LAN connection or Bluetooth Classic for paired devices. Only relevant fields are shown.",
        L"Advanced tools are optional. Voxel profiles are per-ROM; plug-in policy changes require a restart."}};
    const auto index = static_cast<std::size_t>(section);
    if (state.settings_section_description != nullptr) {
        SetWindowTextW(state.settings_section_description,
                       descriptions[index]);
        ShowWindow(state.settings_section_description,
                   settings ? SW_SHOW : SW_HIDE);
    }
    for (std::size_t position = 0; position < state.settings_sections.size();
         ++position) {
        ShowWindow(state.settings_sections[position],
                   settings ? SW_SHOW : SW_HIDE);
        InvalidateRect(state.settings_sections[position], nullptr, TRUE);
    }
    const auto show = [settings, section](const HWND control,
                                           const State::SettingsSection target) {
        if (control != nullptr) {
            ShowWindow(control, settings && section == target ? SW_SHOW
                                                               : SW_HIDE);
        }
    };
    show(state.palette_label, State::SettingsSection::general);
    show(state.palette, State::SettingsSection::general);
    show(state.video_label, State::SettingsSection::general);
    show(state.video, State::SettingsSection::general);
    show(state.hardware_model_label, State::SettingsSection::general);
    show(state.hardware_model, State::SettingsSection::general);
    show(state.audio_enabled, State::SettingsSection::general);

    show(state.controls_label, State::SettingsSection::controls);
    show(state.controls_instruction, State::SettingsSection::controls);
    ShowWindow(state.gameboy_background, SW_HIDE);
    show(state.actions_label, State::SettingsSection::controls);
    show(state.reset_controls, State::SettingsSection::controls);
    for (const auto heading : state.primary_headings) {
        show(heading, State::SettingsSection::controls);
    }
    for (const auto heading : state.secondary_headings) {
        show(heading, State::SettingsSection::controls);
    }
    for (const auto label : state.binding_labels) {
        show(label, State::SettingsSection::controls);
    }
    for (const auto& buttons : state.binding_buttons) {
        for (const auto button : buttons) {
            show(button, State::SettingsSection::controls);
        }
    }
    for (const auto label : state.action_labels) {
        show(label, State::SettingsSection::controls);
    }
    for (const auto button : state.action_buttons) {
        show(button, State::SettingsSection::controls);
    }

    show(state.link_heading, State::SettingsSection::link);
    show(state.link_transport_label, State::SettingsSection::link);
    show(state.link_transport, State::SettingsSection::link);

    show(state.voxel_heading, State::SettingsSection::advanced);
    show(state.voxel_fingerprint_label, State::SettingsSection::advanced);
    show(state.voxel_preview, State::SettingsSection::advanced);
    for (const auto label : state.voxel_labels) {
        show(label, State::SettingsSection::advanced);
    }
    for (const auto edit : state.voxel_edits) {
        show(edit, State::SettingsSection::advanced);
    }
    show(state.voxel_save, State::SettingsSection::advanced);
    show(state.voxel_reset, State::SettingsSection::advanced);
    // Voxel edits are committed with the page-level Apply action; keep the
    // old per-profile save control out of the new staged-settings flow.
    ShowWindow(state.voxel_save, SW_HIDE);
    if (!state.voxel_available) {
        ShowWindow(state.voxel_heading, SW_HIDE);
        ShowWindow(state.voxel_fingerprint_label, SW_HIDE);
        ShowWindow(state.voxel_preview, SW_HIDE);
        for (const auto label : state.voxel_labels) ShowWindow(label, SW_HIDE);
        for (const auto edit : state.voxel_edits) ShowWindow(edit, SW_HIDE);
        ShowWindow(state.voxel_save, SW_HIDE);
        ShowWindow(state.voxel_reset, SW_HIDE);
    }
    show(state.plugin_heading, State::SettingsSection::advanced);
    show(state.plugin_status, State::SettingsSection::advanced);
    show(state.plugin_discovery, State::SettingsSection::advanced);
    show(state.plugin_require_allowlist, State::SettingsSection::advanced);
    show(state.plugin_require_capability_allowlist,
         State::SettingsSection::advanced);
    update_link_control_state(state);
}

std::wstring binding_name(const std::int64_t value) {
    if (value == SDLK_UNKNOWN) return L"Unassigned";
    if (value == SDLK_LSHIFT) return L"Left Shift";
    if (value == SDLK_GRAVE) return L"Grave";
    return widen(SDL_GetKeyName(static_cast<SDL_Keycode>(value)));
}

std::wstring shortcuts_text(const State& state) {
    std::wostringstream text;
    text << L"GAMEPLAY CONTROLS\r\n";
    for (std::size_t index = 0; index < control_names.size(); ++index) {
        text << control_names[index] << L": "
             << binding_name(state.result.keyboard_bindings[index][0]);
        const auto secondary = state.result.keyboard_bindings[index][1];
        if (secondary != SDLK_UNKNOWN) {
            text << L" / " << binding_name(secondary);
        }
        text << L"\r\n";
    }
    text << L"\r\nCONFIGURABLE EMULATOR SHORTCUTS\r\n";
    for (std::size_t index = 0; index < action_names.size(); ++index) {
        text << action_names[index] << L": "
             << binding_name(state.result.action_bindings[index]) << L"\r\n";
    }
    text <<
        L"\r\nGENERAL\r\n"
        L"F1: Open this shortcuts reference\r\n"
        L"Space: Pause or resume\r\n"
        L"Ctrl+R: Reset the current game\r\n"
        L"Ctrl+O: Open a ROM\r\n"
        L"Ctrl+L: Open the game library\r\n"
        L"Ctrl+K: Configure controls\r\n"
        L"Ctrl+P: Choose the display palette\r\n"
        L"Ctrl+G: Open the GameShark cheat manager\r\n"
        L"Ctrl+1 through Ctrl+9: Open a recent ROM\r\n"
        L"F11: Toggle fullscreen in the game window\r\n"
        L"F12: Open or close the debugger from the game window\r\n"
        L"Escape: Close with confirmation\r\n"
        L"\r\nDEBUGGER\r\n"
        L"F5: Run or pause\r\n"
        L"F6: Start or stop input recording\r\n"
        L"F7: Replay the latest input movie\r\n"
        L"F8: Open the TAS frame editor\r\n"
        L"F9: Open the live sprite editor\r\n"
        L"F10: Step one CPU instruction\r\n"
        L"F11: Step one frame (debugger only)\r\n"
        L"F12 or Escape: Close the debugger\r\n"
        L"Click a CPU register: Edit its hexadecimal value\r\n"
        L"\r\nTAS FRAME EDITOR\r\n"
        L"Up / Down: Select a frame\r\n"
        L"Home / Ctrl+End: Jump to first / last frame\r\n"
        L"Page Up / Page Down: Move by a page\r\n"
        L"Insert: Insert an empty frame\r\n"
        L"Delete: Delete the selected frame\r\n"
        L"End: Append an empty frame\r\n"
        L"Ctrl+Z / Ctrl+Y: Undo / redo\r\n"
        L"Ctrl+C / Ctrl+V / Ctrl+D: Copy / paste / duplicate\r\n"
        L"Backspace: Clear selected frame; drag to paint cells\r\n"
        L"Ctrl+N: Start a timeline from the current state\r\n"
        L"Ctrl+S: Save the timeline\r\n"
        L"F7: Save and run the timeline\r\n"
        L"\r\nLIVE SPRITE EDITOR\r\n"
        L"1 through 4: Select a color index\r\n"
        L"Left mouse: Paint; right mouse: erase to color 0\r\n"
        L"Ctrl+Z: Undo the previous stroke\r\n"
        L"Delete: Clear the selected tile\r\n"
        L"B: Switch CGB VRAM bank\r\n"
        L"Ctrl+S: Save a GBB tile patch\r\n"
        L"Ctrl+O: Import the latest GBB tile patch\r\n"
        L"Ctrl+E: Export a standard IPS patch\r\n"
        L"F9 or Escape: Close the sprite editor\r\n";
    text <<
        L"\r\nGAMESHARK CHEAT MANAGER\r\n"
        L"Ctrl+G: Open the current ROM's cheat manager\r\n"
        L"Space: Toggle the selected cheat\r\n"
        L"Delete: Remove the selected cheat\r\n"
        L"Fetch for ROM: Import its Libretro archive entries\r\n";
    return text.str();
}

void refresh_binding_buttons(State& state) {
    for (std::size_t index = 0; index < state.binding_buttons.size(); ++index) {
        for (std::size_t slot = 0;
             slot < state.binding_buttons[index].size(); ++slot) {
            auto text = binding_name(state.result.keyboard_bindings[index][slot]);
            if (state.capturing_binding ==
                std::optional<State::CapturingBinding>{
                    State::CapturingBinding{false, index, slot}}) {
                text = L"Press a key...";
            }
            SetWindowTextW(state.binding_buttons[index][slot], text.c_str());
        }
    }
    for (std::size_t index = 0; index < state.action_buttons.size(); ++index) {
        auto text = binding_name(state.result.action_bindings[index]);
        if (state.capturing_binding ==
            std::optional<State::CapturingBinding>{
                State::CapturingBinding{true, index, 0}}) {
            text = L"Press a key...";
        }
        SetWindowTextW(state.action_buttons[index], text.c_str());
    }
    if (state.shortcuts_text != nullptr) {
        const auto text = shortcuts_text(state);
        SetWindowTextW(state.shortcuts_text, text.c_str());
    }
}

constexpr std::array<const wchar_t*, 15> voxel_profile_names{{
    L"Depth scale", L"Camera pitch", L"Camera yaw", L"Zoom",
    L"Perspective", L"Sprite depth", L"Lighting", L"Popup parallax",
    L"Popup object height", L"Popup sprite height", L"Popup card thickness",
    L"Popup sprite thickness", L"Popup HUD top rows", L"Popup HUD bottom rows",
    L"Framebuffer facade"}};

std::wstring voxel_float_text(const float value) {
    std::wostringstream text;
    text.setf(std::ios::fixed);
    text.precision(3);
    text << value;
    return text.str();
}

bool parse_voxel_edit(const HWND edit, float& target) {
    std::array<wchar_t, 128> buffer{};
    const auto length = GetWindowTextW(edit, buffer.data(),
                                       static_cast<int>(buffer.size()));
    if (length <= 0) return false;
    try {
        std::size_t consumed = 0;
        const auto value = std::stof(narrow(std::wstring(buffer.data(),
                                                         static_cast<std::size_t>(length))),
                                     &consumed);
        const auto text = narrow(std::wstring(buffer.data(),
                                               static_cast<std::size_t>(length)));
        if (consumed != text.size() || !std::isfinite(value)) return false;
        target = value;
        return true;
    } catch (...) {
        return false;
    }
}

void invalidate_voxel_preview(State& state) {
    if (state.voxel_preview == nullptr) return;
    InvalidateRect(state.voxel_preview, nullptr, TRUE);
    UpdateWindow(state.voxel_preview);
}

void refresh_voxel_profile_controls(State& state) {
    if (state.voxel_fingerprint == 0) {
        SetWindowTextW(state.voxel_fingerprint_label,
                       L"No active ROM. Start a game to edit its profile.");
        for (const auto edit : state.voxel_edits) EnableWindow(edit, FALSE);
        EnableWindow(state.voxel_save, FALSE);
        EnableWindow(state.voxel_reset, FALSE);
        invalidate_voxel_preview(state);
        return;
    }
    std::wostringstream fingerprint;
    fingerprint << L"ROM profile: 0x" << std::hex << state.voxel_fingerprint;
    SetWindowTextW(state.voxel_fingerprint_label, fingerprint.str().c_str());
    for (const auto edit : state.voxel_edits) EnableWindow(edit, TRUE);
    EnableWindow(state.voxel_save, TRUE);
    EnableWindow(state.voxel_reset, TRUE);
    const std::array<float, 15> values{{
        state.voxel_profile.depth_scale, state.voxel_profile.camera_pitch,
        state.voxel_profile.camera_yaw, state.voxel_profile.zoom,
        state.voxel_profile.perspective, state.voxel_profile.sprite_depth,
        state.voxel_profile.lighting, state.voxel_profile.popup_parallax,
        state.voxel_profile.popup_object_height,
        state.voxel_profile.popup_sprite_height,
        state.voxel_profile.popup_card_thickness,
        state.voxel_profile.popup_sprite_thickness,
        static_cast<float>(state.voxel_profile.popup_hud_top_rows),
        static_cast<float>(state.voxel_profile.popup_hud_bottom_rows),
        state.voxel_profile.framebuffer_facade ? 1.0F : 0.0F}};
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index == 14) {
            set_dashboard_checkbox_checked(
                state.voxel_edits[index], values[index] >= 0.5F);
        } else {
            SetWindowTextW(state.voxel_edits[index],
                           voxel_float_text(values[index]).c_str());
        }
    }
    invalidate_voxel_preview(state);
}

bool read_voxel_profile_controls(State& state) {
    std::array<float, 15> values{{
        state.voxel_profile.depth_scale, state.voxel_profile.camera_pitch,
        state.voxel_profile.camera_yaw, state.voxel_profile.zoom,
        state.voxel_profile.perspective, state.voxel_profile.sprite_depth,
        state.voxel_profile.lighting, state.voxel_profile.popup_parallax,
        state.voxel_profile.popup_object_height,
        state.voxel_profile.popup_sprite_height,
        state.voxel_profile.popup_card_thickness,
        state.voxel_profile.popup_sprite_thickness,
        static_cast<float>(state.voxel_profile.popup_hud_top_rows),
        static_cast<float>(state.voxel_profile.popup_hud_bottom_rows),
        state.voxel_profile.framebuffer_facade ? 1.0F : 0.0F}};
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index == 14) {
            values[index] = dashboard_checkbox_checked(
                                state.voxel_edits[index])
                                ? 1.0F
                                : 0.0F;
        } else if (!parse_voxel_edit(state.voxel_edits[index], values[index])) {
            return false;
        }
    }
    state.voxel_profile.depth_scale = values[0];
    state.voxel_profile.camera_pitch = values[1];
    state.voxel_profile.camera_yaw = values[2];
    state.voxel_profile.zoom = values[3];
    state.voxel_profile.perspective = values[4];
    state.voxel_profile.sprite_depth = values[5];
    state.voxel_profile.lighting = values[6];
    state.voxel_profile.popup_parallax = values[7];
    state.voxel_profile.popup_object_height = values[8];
    state.voxel_profile.popup_sprite_height = values[9];
    state.voxel_profile.popup_card_thickness = values[10];
    state.voxel_profile.popup_sprite_thickness = values[11];
    state.voxel_profile.popup_hud_top_rows = static_cast<std::uint32_t>(
        std::max(0.0F, values[12]));
    state.voxel_profile.popup_hud_bottom_rows = static_cast<std::uint32_t>(
        std::max(0.0F, values[13]));
    state.voxel_profile.framebuffer_facade = values[14] >= 0.5F;
    return true;
}

bool reserved_binding_key(const State& state, const SDL_Keycode key) {
    switch (key) {
    case SDLK_ESCAPE:
    case SDLK_SPACE:
    case SDLK_F1:
    case SDLK_F11: return true;
    default: break;
    }
    if (!state.capturing_binding || state.capturing_binding->action) {
        return false;
    }
    return std::find(state.result.action_bindings.begin(),
                     state.result.action_bindings.end(), key) !=
           state.result.action_bindings.end();
}

SDL_Keycode keycode_from_windows(const WPARAM virtual_key,
                                 const LPARAM key_data) {
    if (virtual_key >= 'A' && virtual_key <= 'Z') {
        return static_cast<SDL_Keycode>(SDLK_A + virtual_key - 'A');
    }
    if (virtual_key >= '0' && virtual_key <= '9') {
        return static_cast<SDL_Keycode>(SDLK_0 + virtual_key - '0');
    }
    if (virtual_key >= VK_F1 && virtual_key <= VK_F24) {
        return static_cast<SDL_Keycode>(SDLK_F1 + virtual_key - VK_F1);
    }
    switch (virtual_key) {
    case VK_BACK: return SDLK_BACKSPACE;
    case VK_TAB: return SDLK_TAB;
    case VK_RETURN: return SDLK_RETURN;
    case VK_ESCAPE: return SDLK_ESCAPE;
    case VK_SPACE: return SDLK_SPACE;
    case VK_PRIOR: return SDLK_PAGEUP;
    case VK_NEXT: return SDLK_PAGEDOWN;
    case VK_END: return SDLK_END;
    case VK_HOME: return SDLK_HOME;
    case VK_LEFT: return SDLK_LEFT;
    case VK_UP: return SDLK_UP;
    case VK_RIGHT: return SDLK_RIGHT;
    case VK_DOWN: return SDLK_DOWN;
    case VK_INSERT: return SDLK_INSERT;
    case VK_DELETE: return SDLK_DELETE;
    case VK_SHIFT: {
        const auto scan_code = static_cast<UINT>((key_data >> 16) & 0xFF);
        return scan_code == MapVirtualKeyW(VK_RSHIFT, MAPVK_VK_TO_VSC)
                   ? SDLK_RSHIFT
                   : SDLK_LSHIFT;
    }
    case VK_CONTROL:
        return (key_data & (LPARAM{1} << 24)) != 0 ? SDLK_RCTRL : SDLK_LCTRL;
    case VK_MENU:
        return (key_data & (LPARAM{1} << 24)) != 0 ? SDLK_RALT : SDLK_LALT;
    case VK_LSHIFT: return SDLK_LSHIFT;
    case VK_RSHIFT: return SDLK_RSHIFT;
    case VK_LCONTROL: return SDLK_LCTRL;
    case VK_RCONTROL: return SDLK_RCTRL;
    case VK_LMENU: return SDLK_LALT;
    case VK_RMENU: return SDLK_RALT;
    default: break;
    }
    std::array<wchar_t, 8> translated{};
    std::array<BYTE, 256> keyboard_state{};
    GetKeyboardState(keyboard_state.data());
    const auto scan_code = static_cast<UINT>((key_data >> 16) & 0xFF);
    if (ToUnicode(static_cast<UINT>(virtual_key), scan_code,
                  keyboard_state.data(), translated.data(),
                  static_cast<int>(translated.size()), 0) == 1) {
        return SDL_GetKeyFromName(narrow(translated.data()).c_str());
    }
    return SDLK_UNKNOWN;
}

void assign_captured_binding(State& state, const SDL_Keycode key) {
    if (!state.capturing_binding) return;
    const auto capture = *state.capturing_binding;
    const auto current = capture.action
                             ? state.result.action_bindings[capture.index]
                             : state.result.keyboard_bindings[capture.index]
                                                               [capture.slot];
    if (current == key) {
        state.capturing_binding.reset();
        SetWindowTextW(state.controls_instruction,
                       L"Binding unchanged. Click another binding to continue.");
        refresh_binding_buttons(state);
        return;
    }
    if (capture.action) {
        for (auto& action : state.result.action_bindings) {
            if (action == key) action = SDLK_UNKNOWN;
        }
        for (auto& bindings : state.result.keyboard_bindings) {
            if (bindings[0] == key) {
                bindings[0] = bindings[1];
                bindings[1] = SDLK_UNKNOWN;
            } else if (bindings[1] == key) {
                bindings[1] = SDLK_UNKNOWN;
            }
        }
        state.result.action_bindings[capture.index] = key;
        state.result.action_bindings_changed = true;
        state.result.keyboard_bindings_changed = true;
    } else {
        const auto target_index = capture.index;
        const auto target_slot = capture.slot;
        for (auto& bindings : state.result.keyboard_bindings) {
            if (bindings[0] == key) {
                bindings[0] = bindings[1];
                bindings[1] = SDLK_UNKNOWN;
            } else if (bindings[1] == key) {
                bindings[1] = SDLK_UNKNOWN;
            }
        }
        state.result.keyboard_bindings[target_index][target_slot] = key;
        state.result.keyboard_bindings_changed = true;
    }
    state.capturing_binding.reset();
    mark_settings_dirty(state);
    SetWindowTextW(state.controls_instruction,
                   L"Click a slot, press a key, or press Delete to clear it.");
    refresh_binding_buttons(state);
}

void draw_gameboy_background(const DRAWITEMSTRUCT& item) {
    const auto canvas = CreateSolidBrush(RGB(13, 18, 27));
    const auto device = CreateSolidBrush(RGB(31, 42, 57));
    const auto screen = CreateSolidBrush(RGB(35, 104, 123));
    const auto red = CreateSolidBrush(RGB(237, 77, 132));
    RECT body{6, 2, item.rcItem.right - 6, item.rcItem.bottom - 2};
    FillRect(item.hDC, &item.rcItem, canvas);
    const auto old_pen = SelectObject(item.hDC, GetStockObject(NULL_PEN));
    const auto old_brush = SelectObject(item.hDC, device);
    RoundRect(item.hDC, body.left, body.top, body.right, body.bottom, 24, 24);
    RECT display{190, 14, item.rcItem.right - 190, 64};
    FillRect(item.hDC, &display, screen);
    // Keep the D-pad distinct from the dark label backgrounds used by the
    // native controls layered over this artwork.
    const auto dpad = CreateSolidBrush(RGB(72, 105, 119));
    SelectObject(item.hDC, dpad);
    RECT dpad_horizontal{40, 112, 122, 140};
    RECT dpad_vertical{67, 85, 95, 167};
    FillRect(item.hDC, &dpad_horizontal, dpad);
    FillRect(item.hDC, &dpad_vertical, dpad);
    SelectObject(item.hDC, red);
    Ellipse(item.hDC, item.rcItem.right - 115, 92,
            item.rcItem.right - 75, 132);
    Ellipse(item.hDC, item.rcItem.right - 165, 125,
            item.rcItem.right - 125, 165);
    SelectObject(item.hDC, old_brush);
    SelectObject(item.hDC, old_pen);
    DeleteObject(dpad);
    DeleteObject(device);
    DeleteObject(screen);
    DeleteObject(red);
    DeleteObject(canvas);
}

struct PreviewPoint {
    int x{};
    int y{};
};

PreviewPoint project_voxel_point(const float x, const float y, const float z,
                                 const gbb::VoxelProfile& profile,
                                 const int width, const int height) {
    constexpr float pi = 3.14159265358979323846F;
    const auto yaw = profile.camera_yaw * pi / 180.0F;
    const auto pitch = profile.camera_pitch * pi / 180.0F;
    const auto yaw_x = std::cos(yaw) * x - std::sin(yaw) * y;
    const auto yaw_depth = std::sin(yaw) * x + std::cos(yaw) * y;
    const auto vertical = std::cos(pitch) * z - std::sin(pitch) * yaw_depth;
    const auto denominator = (std::max)(
        0.2F, 1.0F + profile.perspective * yaw_depth * 28.0F);
    const auto scale = std::clamp(profile.zoom, 0.25F, 4.0F) *
                       static_cast<float>((std::min)(width, height)) * 0.34F;
    return PreviewPoint{
        static_cast<int>(std::lround(static_cast<float>(width) * 0.5F +
                                     yaw_x * scale / denominator)),
        static_cast<int>(std::lround(static_cast<float>(height) * 0.64F -
                                     vertical * scale / denominator))};
}

void draw_voxel_preview(const DRAWITEMSTRUCT& item, const State& state) {
    const auto background = CreateSolidBrush(RGB(11, 18, 29));
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);

    auto profile = state.voxel_profile;
    profile.depth_scale = std::clamp(profile.depth_scale, 0.0F, 8.0F);
    profile.camera_pitch = std::clamp(profile.camera_pitch, -80.0F, 80.0F);
    profile.camera_yaw = std::clamp(profile.camera_yaw, -180.0F, 180.0F);
    profile.zoom = std::clamp(profile.zoom, 0.25F, 4.0F);
    profile.perspective = std::clamp(profile.perspective, 0.0F, 0.02F);
    profile.sprite_depth = std::clamp(profile.sprite_depth, 0.0F, 64.0F);
    profile.lighting = std::clamp(profile.lighting, 0.1F, 2.0F);

    const auto width = item.rcItem.right - item.rcItem.left;
    const auto height = item.rcItem.bottom - item.rcItem.top;
    const auto grid_pen = CreatePen(PS_SOLID, 1, RGB(49, 94, 112));
    const auto old_pen = SelectObject(item.hDC, grid_pen);
    const auto old_brush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    for (int line = -2; line <= 2; ++line) {
        const auto left = project_voxel_point(-1.8F, static_cast<float>(line) *
                                                       0.55F, 0.0F,
                                               profile, width, height);
        const auto right = project_voxel_point(1.8F, static_cast<float>(line) *
                                                        0.55F, 0.0F,
                                                profile, width, height);
        MoveToEx(item.hDC, left.x, left.y, nullptr);
        LineTo(item.hDC, right.x, right.y);
    }
    for (int line = -3; line <= 3; ++line) {
        const auto near_point = project_voxel_point(
            static_cast<float>(line) * 0.55F, -1.35F, 0.0F, profile, width,
            height);
        const auto far_point = project_voxel_point(
            static_cast<float>(line) * 0.55F, 1.35F, 0.0F, profile, width,
            height);
        MoveToEx(item.hDC, near_point.x, near_point.y, nullptr);
        LineTo(item.hDC, far_point.x, far_point.y);
    }

    const std::array<std::array<float, 3>, 7> cubes{{
        {{-1.15F, 0.35F, 0.22F}}, {{-0.55F, -0.28F, 0.32F}},
        {{0.05F, 0.20F, 0.46F}}, {{0.68F, -0.35F, 0.28F}},
        {{1.12F, 0.34F, 0.38F}}, {{-0.18F, 0.83F, 0.22F}},
        {{0.56F, 0.76F, 0.30F}}}};
    const std::array<COLORREF, 7> colors{{RGB(0, 164, 205), RGB(44, 107, 153),
                                          RGB(237, 77, 132), RGB(79, 176, 133),
                                          RGB(245, 179, 66), RGB(137, 113, 214),
                                          RGB(224, 94, 104)}};
    const auto extrusion = (std::max)(
        0.05F, profile.depth_scale * 0.34F + profile.sprite_depth * 0.004F);
    for (std::size_t index = 0; index < cubes.size(); ++index) {
        const auto& cube = cubes[index];
        const auto x = cube[0];
        const auto y = cube[1];
        const auto z = cube[2];
        const auto w = 0.34F;
        const auto h = 0.30F + z;
        const auto p0 = project_voxel_point(x - w, y - w, 0.0F, profile,
                                            width, height);
        const auto p1 = project_voxel_point(x + w, y - w, 0.0F, profile,
                                            width, height);
        const auto p2 = project_voxel_point(x + w, y + w, 0.0F, profile,
                                            width, height);
        const auto t0 = project_voxel_point(x - w, y - w, h * extrusion,
                                            profile, width, height);
        const auto t1 = project_voxel_point(x + w, y - w, h * extrusion,
                                            profile, width, height);
        const auto t2 = project_voxel_point(x + w, y + w, h * extrusion,
                                            profile, width, height);
        const auto t3 = project_voxel_point(x - w, y + w, h * extrusion,
                                            profile, width, height);
        const auto fill = CreateSolidBrush(colors[index]);
        SelectObject(item.hDC, fill);
        const std::array<POINT, 4> side{
            POINT{p0.x, p0.y}, POINT{p1.x, p1.y}, POINT{t1.x, t1.y},
            POINT{t0.x, t0.y}};
        Polygon(item.hDC, side.data(), static_cast<int>(side.size()));
        const std::array<POINT, 4> front{
            POINT{p1.x, p1.y}, POINT{p2.x, p2.y}, POINT{t2.x, t2.y},
            POINT{t1.x, t1.y}};
        Polygon(item.hDC, front.data(), static_cast<int>(front.size()));
        const std::array<POINT, 4> top{
            POINT{t0.x, t0.y}, POINT{t1.x, t1.y}, POINT{t2.x, t2.y},
            POINT{t3.x, t3.y}};
        Polygon(item.hDC, top.data(), static_cast<int>(top.size()));
        SelectObject(item.hDC, old_brush);
        DeleteObject(fill);
    }
    if (profile.framebuffer_facade) {
        const auto facade = CreateSolidBrush(RGB(35, 104, 123));
        SelectObject(item.hDC, facade);
        const auto left = project_voxel_point(-1.45F, -0.85F, 0.05F,
                                              profile, width, height);
        const auto right = project_voxel_point(1.45F, -0.85F, 0.05F,
                                               profile, width, height);
        const auto top = project_voxel_point(1.45F, -0.85F, 0.82F,
                                             profile, width, height);
        const auto far_left = project_voxel_point(-1.45F, -0.85F, 0.82F,
                                                  profile, width, height);
        const std::array<POINT, 4> panel{
            POINT{left.x, left.y}, POINT{right.x, right.y}, POINT{top.x, top.y},
            POINT{far_left.x, far_left.y}};
        Polygon(item.hDC, panel.data(), static_cast<int>(panel.size()));
        SelectObject(item.hDC, old_brush);
        DeleteObject(facade);
    }
    SelectObject(item.hDC, old_brush);
    SelectObject(item.hDC, old_pen);
    DeleteObject(grid_pen);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, RGB(185, 216, 232));
    RECT label{8, 8, width - 8, 26};
    DrawTextW(item.hDC, profile.framebuffer_facade ? L"LIVE PREVIEW - FACADE"
                                                   : L"LIVE PREVIEW - MESH ONLY",
              -1, &label, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
}

LRESULT CALLBACK table_header_subclass(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass_id, DWORD_PTR reference) {
    auto* state = reinterpret_cast<State*>(reference);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT && state != nullptr) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const auto background = CreateSolidBrush(RGB(20, 30, 44));
        FillRect(dc, &client, background);
        DeleteObject(background);
        const auto font = state->ui_font != nullptr
                              ? state->ui_font
                              : reinterpret_cast<HFONT>(
                                    GetStockObject(DEFAULT_GUI_FONT));
        const auto old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(185, 216, 232));
        const auto divider = CreatePen(PS_SOLID, 1, RGB(43, 57, 75));
        const auto old_pen = SelectObject(dc, divider);
        const auto count = Header_GetItemCount(window);
        for (int index = 0; index < count; ++index) {
            RECT rectangle{};
            if (!Header_GetItemRect(window, index, &rectangle)) continue;
            wchar_t label[128]{};
            HDITEMW item{};
            item.mask = HDI_TEXT;
            item.pszText = label;
            item.cchTextMax = static_cast<int>(std::size(label));
            if (!SendMessageW(window, HDM_GETITEMW,
                              static_cast<WPARAM>(index),
                              reinterpret_cast<LPARAM>(&item))) {
                continue;
            }
            if (state->library_sort_column == index) {
                const auto suffix = state->library_sort_descending ? L"  v" : L"  ^";
                wcsncat_s(label, std::size(label), suffix,
                          std::size(label) - wcslen(label) - 1);
            }
            rectangle.left += 12;
            DrawTextW(dc, label, -1, &rectangle,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            MoveToEx(dc, rectangle.right - 12, rectangle.top + 4, nullptr);
            LineTo(dc, rectangle.right - 12, rectangle.bottom - 4);
        }
        SelectObject(dc, old_pen);
        DeleteObject(divider);
        SelectObject(dc, old_font);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, table_header_subclass, subclass_id);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void refresh_library_actions(State& state);
void refresh_library_list(State& state);
void layout_dashboard(State& state);
void update_artwork_retry_visibility(State& state);
void start_artwork_resolution(State& state);

void show_page(State& state, const State::Page page) {
    state.page = page;
    const auto library = page == State::Page::library;
    const auto settings = page == State::Page::settings;
    const auto shortcuts = page == State::Page::shortcuts;
    if (!settings && state.capturing_binding) {
        state.capturing_binding.reset();
        refresh_binding_buttons(state);
    }
    ShowWindow(state.list, library ? SW_SHOW : SW_HIDE);
    ShowWindow(state.search_label, library ? SW_SHOW : SW_HIDE);
    ShowWindow(state.search, library ? SW_SHOW : SW_HIDE);
    ShowWindow(state.search_summary, library ? SW_SHOW : SW_HIDE);
    ShowWindow(state.search_clear,
               library && !state.library_filter.empty() ? SW_SHOW : SW_HIDE);
    ShowWindow(state.artwork_status, library ? SW_SHOW : SW_HIDE);
    ShowWindow(state.artwork_retry,
               library && state.artwork_failed.load(std::memory_order_relaxed) != 0
                   ? SW_SHOW
                   : SW_HIDE);
    if (state.library_empty != nullptr) {
        const auto empty = library && ListView_GetItemCount(state.list) == 0;
        ShowWindow(state.library_empty, empty ? SW_SHOW : SW_HIDE);
    }
    ShowWindow(state.play, library ? SW_SHOW : SW_HIDE);
    ShowWindow(state.open, library ? SW_SHOW : SW_HIDE);
    ShowWindow(state.remove, library ? SW_SHOW : SW_HIDE);
    ShowWindow(state.resume, library && state.can_resume ? SW_SHOW : SW_HIDE);
    ShowWindow(state.settings_heading, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.settings_status, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.settings_apply, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.settings_cancel, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.palette_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.palette, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.video_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.video, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.hardware_model_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.hardware_model, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.audio_enabled, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.controls_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.controls_instruction, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.actions_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.gameboy_background, settings ? SW_SHOW : SW_HIDE);
    for (const auto heading : state.primary_headings) {
        ShowWindow(heading, settings ? SW_SHOW : SW_HIDE);
    }
    for (const auto heading : state.secondary_headings) {
        ShowWindow(heading, settings ? SW_SHOW : SW_HIDE);
    }
    for (const auto label : state.binding_labels) {
        ShowWindow(label, settings ? SW_SHOW : SW_HIDE);
    }
    for (const auto& buttons : state.binding_buttons) {
        for (const auto button : buttons) {
            ShowWindow(button, settings ? SW_SHOW : SW_HIDE);
        }
    }
    for (const auto label : state.action_labels) {
        ShowWindow(label, settings ? SW_SHOW : SW_HIDE);
    }
    for (const auto button : state.action_buttons) {
        ShowWindow(button, settings ? SW_SHOW : SW_HIDE);
    }
    ShowWindow(state.reset_controls, settings ? SW_SHOW : SW_HIDE);
    const auto show_voxel = settings && state.voxel_available;
    ShowWindow(state.voxel_heading, show_voxel ? SW_SHOW : SW_HIDE);
    ShowWindow(state.voxel_fingerprint_label, show_voxel ? SW_SHOW : SW_HIDE);
    ShowWindow(state.voxel_preview, show_voxel ? SW_SHOW : SW_HIDE);
    for (const auto label : state.voxel_labels) {
        ShowWindow(label, show_voxel ? SW_SHOW : SW_HIDE);
    }
    for (const auto edit : state.voxel_edits) {
        ShowWindow(edit, show_voxel ? SW_SHOW : SW_HIDE);
    }
    ShowWindow(state.voxel_save, show_voxel ? SW_SHOW : SW_HIDE);
    ShowWindow(state.voxel_reset, show_voxel ? SW_SHOW : SW_HIDE);
    ShowWindow(state.plugin_heading, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.plugin_status, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.plugin_discovery, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.plugin_require_allowlist, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.plugin_require_capability_allowlist, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_heading, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_transport_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_transport, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_remote_host_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_remote_host, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_remote_bind_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_remote_bind, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_remote_port_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_remote_port, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_lan_discovery, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_bluetooth_address_label,
               settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_bluetooth_address, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_bluetooth_choose, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_bluetooth_uuid_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_bluetooth_uuid, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.link_diagnostics, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.shortcuts_heading, shortcuts ? SW_SHOW : SW_HIDE);
    ShowWindow(state.shortcuts_text, shortcuts ? SW_SHOW : SW_HIDE);
    show_settings_section(state);
    layout_dashboard(state);
    if (library) refresh_library_actions(state);
    if (settings) {
        // Refresh the native binding buttons immediately when entering the
        // controls section so their staged values are always visible.
        for (const auto& buttons : state.binding_buttons) {
            for (const auto button : buttons) {
                SetWindowPos(button, HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                InvalidateRect(button, nullptr, TRUE);
                UpdateWindow(button);
            }
        }
        RedrawWindow(state.window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

std::optional<std::size_t> selected_entry_index(const State& state) {
    const auto selected = ListView_GetNextItem(state.list, -1, LVNI_SELECTED);
    if (selected < 0) return std::nullopt;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = selected;
    if (!SendMessageW(state.list, LVM_GETITEMW, 0,
                      reinterpret_cast<LPARAM>(&item))) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(item.lParam);
    return index < state.library->entries().size()
               ? std::optional<std::size_t>{index}
               : std::nullopt;
}

void refresh_library_actions(State& state) {
    const auto has_selection = selected_entry_index(state).has_value();
    if (state.play != nullptr) EnableWindow(state.play, has_selection);
    if (state.remove != nullptr) EnableWindow(state.remove, has_selection);
}

std::wstring lowercase(std::wstring value) {
    for (auto& character : value) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return value;
}

void refresh_library_list(State& state) {
    if (state.list == nullptr || state.library == nullptr) return;
    state.library_filter = lowercase(edit_value(state.search));
    ListView_DeleteAllItems(state.list);
    std::vector<std::size_t> indices;
    indices.reserve(state.library->entries().size());
    for (std::size_t index = 0; index < state.library->entries().size(); ++index) {
        const auto& entry = state.library->entries()[index];
        const auto title = widen(entry.metadata.title);
        const auto filename = entry.path.filename().wstring();
        const auto path = entry.path.wstring();
        const auto searchable = lowercase(title + L" " + filename + L" " + path);
        if (!state.library_filter.empty() &&
            searchable.find(state.library_filter) == std::wstring::npos) {
            continue;
        }
        indices.push_back(index);
    }
    if (state.library_sort_column != 0) {
        std::stable_sort(indices.begin(), indices.end(), [&](const auto left,
                                                              const auto right) {
            if (state.library_sort_column == 4) {
                const auto left_time = state.library->entries()[left].last_played;
                const auto right_time = state.library->entries()[right].last_played;
                // Unknown entries belong at the end in either direction; a
                // newly installed game should not hide older played games.
                if (left_time <= 0 || right_time <= 0) {
                    if (left_time <= 0 && right_time <= 0) return left < right;
                    return left_time > 0;
                }
                if (left_time == right_time) return left < right;
                return state.library_sort_descending ? left_time > right_time
                                                     : left_time < right_time;
            }
            const auto value = [&](const std::size_t index) {
                const auto& entry = state.library->entries()[index];
                switch (state.library_sort_column) {
                case 1: return widen(entry.metadata.title.empty()
                                          ? entry.path.filename().u8string()
                                          : entry.metadata.title);
                case 2: return widen(gameboy::platform_name(entry.metadata.platform));
                case 3: return widen(entry.metadata.language);
                case 4: return formatted_last_played(entry.last_played);
                case 5: return entry.path.wstring();
                default: return std::wstring{};
                }
            };
            const auto left_value = value(left);
            const auto right_value = value(right);
            if (left_value == right_value) return left < right;
            return state.library_sort_descending ? left_value > right_value
                                                 : left_value < right_value;
        });
    }
    for (const auto index : indices) {
        const auto& entry = state.library->entries()[index];
        const auto title = widen(entry.metadata.title);
        const auto row_title = title.empty() ? entry.path.filename().wstring()
                                              : title;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem = ListView_GetItemCount(state.list);
        item.pszText = const_cast<wchar_t*>(L"");
        item.iImage = 0;
        item.lParam = static_cast<LPARAM>(index);
        const auto row = static_cast<int>(SendMessageW(
            state.list, LVM_INSERTITEMW, 0,
            reinterpret_cast<LPARAM>(&item)));
        if (row < 0) continue;
        LVITEMW subitem{};
        subitem.iSubItem = 1;
        subitem.pszText = const_cast<wchar_t*>(row_title.c_str());
        SendMessageW(state.list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(row),
                     reinterpret_cast<LPARAM>(&subitem));
        auto platform_name = widen(gameboy::platform_name(entry.metadata.platform));
        subitem.iSubItem = 2;
        subitem.pszText = const_cast<wchar_t*>(platform_name.c_str());
        SendMessageW(state.list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(row),
                     reinterpret_cast<LPARAM>(&subitem));
        auto language = widen(entry.metadata.language);
        subitem.iSubItem = 3;
        subitem.pszText = const_cast<wchar_t*>(language.c_str());
        SendMessageW(state.list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(row),
                     reinterpret_cast<LPARAM>(&subitem));
        auto last_played = formatted_last_played(entry.last_played);
        subitem.iSubItem = 4;
        subitem.pszText = const_cast<wchar_t*>(last_played.c_str());
        SendMessageW(state.list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(row),
                     reinterpret_cast<LPARAM>(&subitem));
        auto path = entry.path.wstring();
        subitem.iSubItem = 5;
        subitem.pszText = const_cast<wchar_t*>(path.c_str());
        SendMessageW(state.list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(row),
                     reinterpret_cast<LPARAM>(&subitem));
    }
    const auto empty = state.page == State::Page::library &&
                       ListView_GetItemCount(state.list) == 0;
    if (state.library_empty != nullptr) {
        SetWindowTextW(
            state.library_empty,
            state.library_filter.empty()
                ? L"Welcome to Go Bigger Boy.\n\nChoose Open ROM... to add your first game to the library."
                : L"No games match this filter.\n\nTry another title or clear the search field.");
        ShowWindow(state.library_empty, empty ? SW_SHOW : SW_HIDE);
    }
    if (state.search_summary != nullptr) {
        const auto count = std::to_wstring(indices.size());
        const auto summary = count + (indices.size() == 1 ? L" game" : L" games") +
                             (state.library_filter.empty() ? L"" : L" match");
        SetWindowTextW(state.search_summary, summary.c_str());
    }
    if (state.search_clear != nullptr) {
        ShowWindow(state.search_clear,
                   state.page == State::Page::library &&
                           !state.library_filter.empty()
                       ? SW_SHOW
                       : SW_HIDE);
    }
    refresh_library_actions(state);
}

void place_child(HWND child, int x, int y, int width, int height,
                 const int scroll) {
    if (child == nullptr) return;
    SetWindowPos(child, nullptr, x, y - scroll, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void clip_settings_content_children(State& state, const int client_height) {
    for (const auto child : state.settings_content_controls) {
        if (child == nullptr) continue;
        if (state.page != State::Page::settings) {
            if (GetPropW(child, dashboard_content_clip_property) != nullptr) {
                SetWindowRgn(child, nullptr, TRUE);
                RemovePropW(child, dashboard_content_clip_property);
            }
            continue;
        }

        RECT screen_rect{};
        if (!GetWindowRect(child, &screen_rect)) continue;
        POINT corners[2]{{screen_rect.left, screen_rect.top},
                         {screen_rect.right, screen_rect.bottom}};
        MapWindowPoints(nullptr, state.window, corners, 2);
        const auto child_width = static_cast<int>(corners[1].x - corners[0].x);
        const auto child_height =
            static_cast<int>(corners[1].y - corners[0].y);
        if (child_width <= 0 || child_height <= 0) continue;

        const auto child_top = static_cast<int>(corners[0].y);
        const auto child_bottom = static_cast<int>(corners[1].y);
        const auto fully_visible = child_top >= settings_content_top &&
                                   child_bottom <= client_height;
        if (fully_visible) {
            // Remove a previous clipping region when scrolling back into view.
            if (GetPropW(child, dashboard_content_clip_property) != nullptr) {
                SetWindowRgn(child, nullptr, TRUE);
                RemovePropW(child, dashboard_content_clip_property);
            }
            continue;
        }

        // Keep the child at its real position and size while clipping only
        // the part outside the scrolling viewport. Resizing controls to zero
        // height here causes a visible destroy/recreate-like flash on Windows
        // while the scroll position changes.
        const auto visible_top = std::max(settings_content_top, child_top);
        const auto visible_bottom = std::min(client_height, child_bottom);
        const auto local_top = std::clamp(visible_top - child_top, 0,
                                          child_height);
        const auto local_bottom = std::clamp(visible_bottom - child_top, 0,
                                             child_height);
        const auto region = CreateRectRgn(0, local_top, child_width,
                                          std::max(local_top, local_bottom));
        SetWindowRgn(child, region, TRUE);
        SetPropW(child, dashboard_content_clip_property,
                 reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
    }
}

void layout_dashboard(State& state) {
    if (state.window == nullptr) return;
    RECT client{};
    GetClientRect(state.window, &client);
    const auto width = std::max(720L, client.right - client.left);
    const auto height = std::max(520L, client.bottom - client.top);
    const auto content_width = std::max(400L, width - 64L);
    const auto list_top = 240L;
    const auto list_height = std::max(180L, height - 340L);
    place_child(state.search_label, 32, 180, 110, 26, 0);
    place_child(state.search, 154, 176, 400, 28, 0);
    place_child(state.search_summary, 570, 180, 250, 26, 0);
    place_child(state.search_clear, 830, 176, 118, 28, 0);
    place_child(state.list, 32, static_cast<int>(list_top),
                static_cast<int>(content_width),
                static_cast<int>(list_height), 0);
    place_child(state.library_empty, 32, static_cast<int>(list_top + 100),
                static_cast<int>(content_width),
                100, 0);
    place_child(state.artwork_status, 32, 212,
                static_cast<int>(content_width) - 130, 20, 0);
    place_child(state.artwork_retry, static_cast<int>(content_width) - 98, 208,
                98, 28, 0);
    const auto actions_y = list_top + list_height + 20L;
    place_child(state.open, 32, static_cast<int>(actions_y), 150, 44, 0);
    place_child(state.play, 202, static_cast<int>(actions_y), 160, 44, 0);
    place_child(state.remove, 382, static_cast<int>(actions_y), 170, 44, 0);
    place_child(state.resume, 572, static_cast<int>(actions_y), 150, 44, 0);

    const auto content_bottom = settings_content_bottom(state);
    const auto max_scroll = std::max(0L, content_bottom - height + 24L);
    // Showing the settings scrollbar reduces the client width after the
    // initial layout measurement. Reserve that width up front so the rightmost
    // section button and other fixed-width controls stay inside the final
    // client area.
    const auto layout_width = std::max(
        720L, width - (state.page == State::Page::settings && max_scroll > 0
                           ? GetSystemMetrics(SM_CXVSCROLL)
                           : 0L));
    state.settings_scroll = std::clamp(state.settings_scroll, 0,
                                       static_cast<int>(max_scroll));
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS,
                      0, static_cast<int>(content_bottom),
                      static_cast<UINT>(height), state.settings_scroll, 0};
    SetScrollInfo(state.window, SB_VERT, &scroll, TRUE);
    ShowScrollBar(state.window, SB_VERT,
                  state.page == State::Page::settings && max_scroll > 0
                      ? TRUE
                      : FALSE);
    const auto offset = state.settings_scroll;
    place_child(state.settings_heading, 32, 200, 260, 30, 0);
    constexpr std::array<int, 4> section_x{{32, 258, 484, 710}};
    constexpr std::array<int, 4> section_width{{210, 210, 210, 238}};
    for (std::size_t index = 0; index < state.settings_sections.size();
        ++index) {
        const auto available_width =
            std::max(1L, layout_width - section_x[index]);
        place_child(state.settings_sections[index], section_x[index], 235,
                    static_cast<int>(std::min<long>(
                        section_width[index], available_width)),
                    34, 0);
    }
    place_child(state.settings_section_description, 32, 280, 916, 38, 0);

    // General settings stay deliberately small and understandable: one
    // choice per row, with the technical controls kept out of the way.
    place_child(state.settings_status, 510, 112, 440, 20, 0);
    place_child(state.settings_apply, 650, 140, 140, 40, 0);
    place_child(state.settings_cancel, 802, 140, 126, 40, 0);
    place_child(state.palette_label, 32, 350, 150, 26, offset);
    place_child(state.palette, 200, 345, 320, 28, offset);
    place_child(state.video_label, 32, 395, 150, 26, offset);
    place_child(state.video, 200, 390, 320, 28, offset);
    place_child(state.hardware_model_label, 32, 440, 150, 26, offset);
    place_child(state.hardware_model, 200, 435, 320, 28, offset);
    place_child(state.audio_enabled, 32, 490, 360, 34, offset);

    // Controls use a literal table instead of placing buttons over a
    // decorative controller illustration.
    place_child(state.controls_label, 32, 330, 300, 28, offset);
    place_child(state.controls_instruction, 32, 365, 916, 26, offset);
    for (int column = 0; column < 2; ++column) {
        const auto base_x = column == 0 ? 32 : 510;
        place_child(state.primary_headings[static_cast<std::size_t>(column)],
                    base_x + 135, 390, 145, 24, offset);
        place_child(state.secondary_headings[static_cast<std::size_t>(column)],
                    base_x + 285, 390, 145, 24, offset);
    }
    constexpr std::array<std::size_t, 8> order{{2, 1, 0, 3, 4, 5, 6, 7}};
    for (std::size_t position = 0; position < order.size(); ++position) {
        const auto index = order[position];
        const auto column = position < 4 ? 0 : 1;
        const auto row = static_cast<int>(position % 4);
        const auto base_x = column == 0 ? 32 : 510;
        const auto y = 420 + row * 48;
        place_child(state.binding_labels[index], base_x, y + 8, 125, 26, offset);
        for (std::size_t slot = 0; slot < 2; ++slot) {
            place_child(state.binding_buttons[index][slot],
                        base_x + 135 + static_cast<int>(slot) * 150, y, 140, 38,
                        offset);
        }
    }
    place_child(state.actions_label, 32, 650, 300, 28, offset);
    for (std::size_t index = 0; index < state.action_labels.size(); ++index) {
        const auto column = index % 2;
        const auto row = index / 2;
        const auto base_x = column == 0 ? 32 : 510;
        const auto y = 690 + static_cast<int>(row) * 50;
        place_child(state.action_labels[index], base_x, y + 8, 135, 26, offset);
        place_child(state.action_buttons[index], base_x + 150, y, 180, 38,
                    offset);
    }
    place_child(state.reset_controls, 32, 790, 230, 40, offset);

    // Advanced settings use the same compact grid, with plug-ins below the
    // optional per-ROM voxel profile.
    const auto advanced_plugin_y = state.voxel_available ? 620 : 350;
    place_child(state.voxel_heading, 32, 330, 300, 28, offset);
    place_child(state.voxel_fingerprint_label, 350, 333, 598, 24, offset);
    place_child(state.voxel_preview, 300, 380, 180, 150, offset);
    constexpr std::array<int, 15> profile_x{{
        32, 32, 32, 32, 32, 32, 32,
        510, 510, 510, 510, 510, 510, 510, 510}};
    constexpr std::array<int, 15> profile_y{{
        380, 420, 460, 500, 540, 580, 620,
        380, 420, 460, 500, 540, 580, 620, 660}};
    for (std::size_t index = 0; index < state.voxel_labels.size(); ++index) {
        place_child(state.voxel_labels[index], profile_x[index], profile_y[index],
                    120, 24, offset);
        place_child(state.voxel_edits[index], profile_x[index] + 130,
                    profile_y[index] - 2, 130, 28, offset);
    }
    place_child(state.voxel_save, 680, 705, 120, 38, offset);
    place_child(state.voxel_reset, 810, 705, 138, 38, offset);
    place_child(state.plugin_heading, 32, advanced_plugin_y + 120, 320, 28, offset);
    place_child(state.plugin_status, 32, advanced_plugin_y + 155, 916, 72,
                offset);
    place_child(state.plugin_discovery, 32, advanced_plugin_y + 235, 260, 28,
                offset);
    place_child(state.plugin_require_allowlist, 320, advanced_plugin_y + 235,
                320, 28, offset);
    place_child(state.plugin_require_capability_allowlist, 660,
                advanced_plugin_y + 235, 290, 28,
                offset);
    place_child(state.link_heading, 32, 330, 420, 28, offset);
    place_child(state.link_transport_label, 32, 365, 150, 26, offset);
    place_child(state.link_transport, 200, 360, 320, 28, offset);
    place_child(state.link_remote_host_label, 32, 420, 150, 26, offset);
    place_child(state.link_remote_host, 200, 415, 300, 28, offset);
    place_child(state.link_remote_bind_label, 530, 420, 120, 26, offset);
    place_child(state.link_remote_bind, 665, 415, 283, 28, offset);
    place_child(state.link_remote_port_label, 32, 465, 150, 26, offset);
    place_child(state.link_remote_port, 200, 460, 120, 28, offset);
    place_child(state.link_lan_discovery, 350, 460, 300, 28, offset);
    place_child(state.link_bluetooth_address_label, 32, 420, 170, 26,
                offset);
    place_child(state.link_bluetooth_address, 218, 415, 300, 28, offset);
    place_child(state.link_bluetooth_choose, 218, 450, 300, 28, offset);
    place_child(state.link_bluetooth_uuid_label, 530, 420, 120, 26, offset);
    place_child(state.link_bluetooth_uuid, 665, 415, 283, 28, offset);
    place_child(state.link_diagnostics, 32, 515, 330, 28, offset);
    clip_settings_content_children(state, static_cast<int>(height));
    RECT content_rect{0, settings_content_top, static_cast<LONG>(width),
                      static_cast<LONG>(height)};
    InvalidateRect(state.window, &content_rect, FALSE);
}

void set_settings_scroll(State& state, int target) {
    RECT client{};
    GetClientRect(state.window, &client);
    const auto height = std::max(520L, client.bottom - client.top);
    const auto max_scroll = std::max(
        0L, settings_content_bottom(state) - height + 24L);
    target = std::clamp(target, 0, static_cast<int>(max_scroll));
    const auto delta = target - state.settings_scroll;
    if (delta == 0) return;
    RECT viewport{0, settings_content_top, client.right, client.bottom};
    ScrollWindowEx(state.window, 0, -delta, &viewport, nullptr, nullptr,
                   nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE);
    state.settings_scroll = target;
    layout_dashboard(state);
}

void scroll_settings(State& state, const int wheel_delta) {
    const auto direction = wheel_delta > 0 ? -64 : 64;
    set_settings_scroll(state, state.settings_scroll + direction);
}

void finish(State& state, const DashboardResultAction action,
            const std::string& path = {}, const bool collect_settings = true) {
    if (collect_settings && state.link_transport != nullptr) {
        collect_link_settings(state);
    }
    if (collect_settings && state.result.voxel_profile_changed &&
        state.voxel_available && state.voxel_fingerprint != 0 &&
        !gbb::save_voxel_profile(state.voxel_profile_path,
                                 state.voxel_fingerprint,
                                 state.voxel_profile)) {
        MessageBoxW(state.window, L"Could not save voxel-profiles.ini.",
                    L"Apply settings", MB_OK | MB_ICONERROR);
        return;
    }
    save_window_position(state);
    state.result.action = action;
    state.result.rom_path = path;
    state.closing = true;
    state.artwork_download.cancel_requested.store(true,
                                                  std::memory_order_relaxed);
    state.done = true;
    if (state.window != nullptr) KillTimer(state.window, update_poll_timer);
    DestroyWindow(state.window);
}

void cancel_settings(State& state, const DashboardResultAction action) {
    state.result = state.initial_result;
    state.voxel_profile = state.initial_voxel_profile;
    finish(state, action, {}, false);
}

void play_selection(State& state) {
    const auto selected = selected_entry_index(state);
    if (!selected) return;
    finish(state, DashboardResultAction::open_rom,
           state.library->entries()[*selected].path.u8string());
}

void remove_selection(State& state) {
    const auto selected = selected_entry_index(state);
    if (!selected) return;
    const auto answer = MessageBoxW(
        state.window,
        L"Remove this game from the recently played list?\n\n"
        L"The ROM file and saved game will not be deleted.",
        L"Remove recent game", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (answer != IDYES) return;
    state.result.removed_fingerprints.push_back(
        state.library->entries()[*selected].metadata.fingerprint);
    const auto row = ListView_GetNextItem(state.list, -1, LVNI_SELECTED);
    if (row >= 0) ListView_DeleteItem(state.list, row);
}

void open_rom(State& state) {
    std::array<wchar_t, 32768> filename{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = state.window;
    dialog.lpstrFilter =
        L"Game Boy ROMs (*.gb;*.gbc)\0*.gb;*.gbc\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog)) {
        finish(state, DashboardResultAction::open_rom, narrow(filename.data()));
    }
}

bool confirm_exit(HWND window);
void draw_dashboard_button(const DRAWITEMSTRUCT& item, const State& state);
void draw_dashboard_checkbox(const DRAWITEMSTRUCT& item);
void draw_dashboard_combo(const DRAWITEMSTRUCT& item);

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    auto* state = reinterpret_cast<State*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<State*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);

    if (message == windows_dashboard_smoke_close) {
        // The smoke runner must be able to tear down a failed case without
        // getting stuck behind the production confirmation dialog.
        finish(*state, DashboardResultAction::quit);
        return 0;
    }

    if (message == WM_ERASEBKGND) {
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wparam), &client,
                 state->background_brush);
        return 1;
    }
    if (message == WM_SIZE) {
        layout_dashboard(*state);
        return 0;
    }
    if (message == WM_TIMER && wparam == 1) {
        const auto completed = state->artwork_completed.load(
            std::memory_order_relaxed);
        if (completed >= state->artwork_total) {
            const auto failed = state->artwork_failed.load(
                std::memory_order_relaxed);
            const auto text = failed == 0
                                  ? std::wstring{L"Artwork: ready"}
                                  : L"Artwork: ready (" +
                                        std::to_wstring(failed) +
                                        L" unavailable). Click Retry artwork to try again.";
            SetWindowTextW(state->artwork_status, text.c_str());
            KillTimer(window, 1);
            update_artwork_retry_visibility(*state);
        } else {
            const auto text = L"Artwork: loading " +
                              std::to_wstring(completed) + L" of " +
                              std::to_wstring(state->artwork_total) +
                              L"... (close to cancel)";
            SetWindowTextW(state->artwork_status, text.c_str());
        }
        return 0;
    }
    if (message == WM_TIMER && wparam == update_poll_timer) {
        if (state->poll_update && state->poll_update()) {
            finish(*state, DashboardResultAction::update_available);
        }
        return 0;
    }
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        // Settings uses a fixed two-column canvas. Keep its right-hand
        // controls reachable when the user resizes the window.
        limits->ptMinTrackSize.x = dashboard_width;
        limits->ptMinTrackSize.y = 520;
        return 0;
    }
    if (message == WM_VSCROLL) {
        RECT client{};
        GetClientRect(window, &client);
        const auto max_scroll = std::max(
            0L, settings_content_bottom(*state) -
                    static_cast<long>(client.bottom - client.top) + 24L);
        auto next = state->settings_scroll;
        switch (LOWORD(wparam)) {
        case SB_LINEUP: next -= 32; break;
        case SB_LINEDOWN: next += 32; break;
        case SB_PAGEUP: next -= std::max(64L, (client.bottom - client.top) / 2); break;
        case SB_PAGEDOWN: next += std::max(64L, (client.bottom - client.top) / 2); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: next = HIWORD(wparam); break;
        case SB_TOP: next = 0; break;
        case SB_BOTTOM: next = static_cast<int>(max_scroll); break;
        default: break;
        }
        set_settings_scroll(*state, next);
        return 0;
    }
    if (message == WM_MOUSEWHEEL && state->page == State::Page::settings) {
        scroll_settings(*state, GET_WHEEL_DELTA_WPARAM(wparam));
        return 0;
    }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORLISTBOX ||
        message == WM_CTLCOLOREDIT || message == WM_CTLCOLORBTN) {
        const auto dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, RGB(224, 235, 244));
        SetBkColor(dc, RGB(13, 18, 27));
        // A multiline EDIT scrolls its existing pixels. Transparent text
        // backgrounds leave those old rows behind, causing shortcut lines to
        // accumulate on top of each other after scrolling back upward.
        const auto control = reinterpret_cast<HWND>(lparam);
        SetBkMode(dc, control == state->shortcuts_text ? OPAQUE : TRANSPARENT);
        return reinterpret_cast<INT_PTR>(state->background_brush);
    }
    if (message == WM_CTLCOLORSCROLLBAR) {
        const auto dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, RGB(137, 160, 183));
        SetBkColor(dc, RGB(20, 27, 38));
        return reinterpret_cast<INT_PTR>(state->background_brush);
    }

    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        state->capturing_binding) {
        if (wparam == VK_ESCAPE) {
            state->capturing_binding.reset();
            SetWindowTextW(
                state->controls_instruction,
                L"Click a slot, press a key, or press Delete to clear it.");
            refresh_binding_buttons(*state);
            return 0;
        }
        const auto capture = *state->capturing_binding;
        if (wparam == VK_DELETE) {
            if (capture.action) {
                state->result.action_bindings[capture.index] = SDLK_UNKNOWN;
                state->result.action_bindings_changed = true;
            } else {
                state->result.keyboard_bindings[capture.index][capture.slot] =
                    SDLK_UNKNOWN;
                state->result.keyboard_bindings_changed = true;
            }
            state->capturing_binding.reset();
            SetWindowTextW(
                state->controls_instruction,
                L"Binding removed. Click another binding to continue.");
            refresh_binding_buttons(*state);
            mark_settings_dirty(*state);
            return 0;
        }
        const auto key = keycode_from_windows(wparam, lparam);
        if (key == SDLK_UNKNOWN) {
            MessageBoxW(window, L"That key is not supported.",
                        L"Configure controls", MB_OK | MB_ICONWARNING);
            return 0;
        }
        if (reserved_binding_key(*state, key)) {
            MessageBoxW(window,
                        L"That key is already assigned to an emulator shortcut.",
                        L"Configure controls", MB_OK | MB_ICONWARNING);
            return 0;
        }
        assign_captured_binding(*state, key);
        return 0;
    }

    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        wparam == VK_F1 && !state->capturing_binding) {
        show_page(*state, State::Page::shortcuts);
        return 0;
    }

    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        wparam == VK_ESCAPE) {
        if (state->settings_dirty) {
            if (MessageBoxW(
                    window,
                    L"Discard your unsaved settings changes?",
                    L"Unsaved settings",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                cancel_settings(*state,
                                settings_return_action(state->can_resume));
            }
        } else if (confirm_exit(window)) {
            finish(*state, DashboardResultAction::quit);
        }
        return 0;
    }

    if (message == WM_COMMAND) {
        const auto command = LOWORD(wparam);
        if (command == id_search && HIWORD(wparam) == EN_CHANGE) {
            refresh_library_list(*state);
            return 0;
        }
        if (command == id_search_clear && HIWORD(wparam) == BN_CLICKED) {
            SetWindowTextW(state->search, L"");
            refresh_library_list(*state);
            SetFocus(state->search);
            return 0;
        }
        if (command >= id_voxel_first_edit &&
            command < id_voxel_first_edit + 15 && state->voxel_available) {
            const auto index = static_cast<std::size_t>(
                command - id_voxel_first_edit);
            const auto notification = HIWORD(wparam);
            const auto editing = index == 14 ? notification == BN_CLICKED
                                            : notification == EN_CHANGE;
            if (index == 14 && notification == BN_CLICKED) {
                toggle_dashboard_checkbox(state->voxel_edits[index]);
            }
            if (editing && read_voxel_profile_controls(*state)) {
                state->result.voxel_profile_changed = true;
                mark_settings_dirty(*state);
                invalidate_voxel_preview(*state);
            }
            return 0;
        }
        if (command >= id_binding_first &&
            command < id_binding_first + 16) {
            const auto binding = static_cast<std::size_t>(
                command - id_binding_first);
            state->capturing_binding =
                State::CapturingBinding{false, binding / 2, binding % 2};
            SetWindowTextW(
                state->controls_instruction,
                binding % 2 == 0
                    ? L"Press a key for the primary binding (Escape cancels)."
                    : L"Press a key for the secondary binding, or Delete to clear it.");
            refresh_binding_buttons(*state);
            SetFocus(state->window);
            return 0;
        }
        if (command >= id_action_first && command < id_action_first + 4) {
            const auto action = static_cast<std::size_t>(
                command - id_action_first);
            state->capturing_binding = State::CapturingBinding{true, action, 0};
            SetWindowTextW(
                state->controls_instruction,
                L"Press a key for the shortcut (Escape cancels, Delete clears it).");
            refresh_binding_buttons(*state);
            SetFocus(state->window);
            return 0;
        }
        if (command >= id_settings_section_first &&
            command < id_settings_section_first + 4 &&
            HIWORD(wparam) == BN_CLICKED) {
            if (state->capturing_binding) {
                state->capturing_binding.reset();
                refresh_binding_buttons(*state);
            }
            state->settings_scroll = 0;
            state->settings_section = static_cast<State::SettingsSection>(
                command - id_settings_section_first);
            show_settings_section(*state);
            layout_dashboard(*state);
            return 0;
        }
        switch (LOWORD(wparam)) {
        case id_library: show_page(*state, State::Page::library); return 0;
        case id_settings: show_page(*state, State::Page::settings); return 0;
        case id_shortcuts: show_page(*state, State::Page::shortcuts); return 0;
        case id_open: open_rom(*state); return 0;
        case id_play: play_selection(*state); return 0;
        case id_resume: finish(*state, DashboardResultAction::resume); return 0;
        case id_remove: remove_selection(*state); return 0;
        case id_reset_controls:
            if (MessageBoxW(
                    window,
                    L"Restore all controls and emulator shortcuts to their defaults?",
                    L"Reset controls",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                state->result.keyboard_bindings = default_keyboard_bindings();
                state->result.action_bindings = default_action_bindings();
                state->result.keyboard_bindings_changed = true;
                state->result.action_bindings_changed = true;
                state->capturing_binding.reset();
                mark_settings_dirty(*state);
                SetWindowTextW(state->controls_instruction,
                               L"All controls and shortcuts restored to defaults.");
                refresh_binding_buttons(*state);
            }
            return 0;
        case id_palette:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                const auto selected = SendMessageW(state->palette, CB_GETCURSEL,
                                                   0, 0);
                if (selected >= 0) {
                    state->result.palette = static_cast<std::size_t>(selected);
                    state->result.palette_changed = true;
                    mark_settings_dirty(*state);
                }
            }
            return 0;
        case id_video:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                const auto selected = SendMessageW(state->video, CB_GETCURSEL,
                                                   0, 0);
                if (selected >= 0 &&
                    selected < static_cast<LRESULT>(gameboy::video_modes.size())) {
                    state->result.video_mode = gameboy::video_modes[
                        static_cast<std::size_t>(selected)].mode;
                    state->result.video_mode_changed = true;
                    mark_settings_dirty(*state);
                }
            }
            return 0;
        case id_hardware_model:
            // CBN_SELENDOK fires after the drop-down has closed, so the
            // restart notice is tied to the user's confirmed change instead
            // of being deferred until the next ROM is opened.
            if (HIWORD(wparam) == CBN_SELENDOK) {
                const auto selected = SendMessageW(state->hardware_model,
                                                   CB_GETCURSEL, 0, 0);
                if (selected >= 0 && selected < static_cast<LRESULT>(gameboy::selectable_hardware_models.size())) {
                    const auto model = gameboy::selectable_hardware_models[
                        static_cast<std::size_t>(selected)];
                    if (model != state->result.hardware_model) {
                        state->result.hardware_model = model;
                        state->result.hardware_model_changed = true;
                        mark_settings_dirty(*state);
                        SetWindowTextW(
                            state->settings_status,
                            L"Hardware model changed. It will apply to the next game.");
                    }
                }
            }
            return 0;
        case id_audio_enabled:
            if (HIWORD(wparam) == BN_CLICKED) {
                toggle_dashboard_checkbox(state->audio_enabled);
                state->result.audio_enabled =
                    dashboard_checkbox_checked(state->audio_enabled);
                state->result.audio_enabled_changed = true;
                mark_settings_dirty(*state);
            }
            return 0;
        case id_link_transport:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                update_link_control_state(*state);
                mark_settings_dirty(*state);
            }
            return 0;
        case id_link_remote_host:
        case id_link_remote_bind:
        case id_link_bluetooth_address:
        case id_link_bluetooth_uuid:
            if (HIWORD(wparam) == EN_CHANGE) mark_settings_dirty(*state);
            return 0;
        case id_link_bluetooth_choose:
            if (HIWORD(wparam) == BN_CLICKED &&
                choose_bluetooth_device(*state)) {
                mark_settings_dirty(*state);
            }
            return 0;
        case id_link_remote_port:
            if (HIWORD(wparam) == EN_CHANGE) {
                show_link_port_validation(*state);
            }
            return 0;
        case id_link_lan_discovery:
        case id_link_diagnostics:
            if (HIWORD(wparam) == BN_CLICKED) {
                toggle_dashboard_checkbox(
                    command == id_link_lan_discovery
                        ? state->link_lan_discovery
                        : state->link_diagnostics);
                mark_settings_dirty(*state);
            }
            return 0;
        case id_plugin_discovery:
            if (HIWORD(wparam) == BN_CLICKED) {
                toggle_dashboard_checkbox(state->plugin_discovery);
                state->result.plugin_discovery =
                    dashboard_checkbox_checked(state->plugin_discovery);
                state->result.plugin_settings_changed = true;
                mark_settings_dirty(*state);
                SetWindowTextW(
                    state->plugin_status,
                    L"Plugin setting changed. Restart the emulator to reload plugins.");
            }
            return 0;
        case id_plugin_require_allowlist:
            if (HIWORD(wparam) == BN_CLICKED) {
                toggle_dashboard_checkbox(state->plugin_require_allowlist);
                state->result.plugin_require_allowlist =
                    dashboard_checkbox_checked(state->plugin_require_allowlist);
                state->result.plugin_settings_changed = true;
                mark_settings_dirty(*state);
                SetWindowTextW(
                    state->plugin_status,
                    L"Plugin trust policy changed. Restart the emulator to reload plugins.");
            }
            return 0;
        case id_plugin_require_capability_allowlist:
            if (HIWORD(wparam) == BN_CLICKED) {
                toggle_dashboard_checkbox(
                    state->plugin_require_capability_allowlist);
                state->result.plugin_require_capability_allowlist =
                    dashboard_checkbox_checked(
                        state->plugin_require_capability_allowlist);
                state->result.plugin_settings_changed = true;
                mark_settings_dirty(*state);
                SetWindowTextW(
                    state->plugin_status,
                    L"Plugin capability policy changed. Restart the emulator to reload plugins.");
            }
            return 0;
        case id_voxel_save:
            if (!state->voxel_available || state->voxel_fingerprint == 0) {
                return 0;
            }
            if (!read_voxel_profile_controls(*state)) {
                MessageBoxW(window,
                            L"Enter valid numeric values for every voxel profile field.",
                            L"Voxel profile", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (!gbb::save_voxel_profile(state->voxel_profile_path,
                                         state->voxel_fingerprint,
                                         state->voxel_profile)) {
                MessageBoxW(window, L"Could not save voxel-profiles.ini.",
                            L"Voxel profile", MB_OK | MB_ICONERROR);
                return 0;
            }
            state->result.voxel_profile_changed = true;
            mark_settings_dirty(*state);
            SetWindowTextW(state->voxel_fingerprint_label,
                           L"Voxel profile staged. Apply settings to save it.");
            return 0;
        case id_voxel_reset:
            if (!state->voxel_available || state->voxel_fingerprint == 0) {
                return 0;
            }
            state->voxel_profile = gbb::VoxelProfile{};
            refresh_voxel_profile_controls(*state);
            if (!gbb::save_voxel_profile(state->voxel_profile_path,
                                         state->voxel_fingerprint,
                                         state->voxel_profile)) {
                MessageBoxW(window, L"Could not save voxel-profiles.ini.",
                            L"Voxel profile", MB_OK | MB_ICONERROR);
                return 0;
            }
            state->result.voxel_profile_changed = true;
            mark_settings_dirty(*state);
            SetWindowTextW(state->voxel_fingerprint_label,
                           L"Default voxel profile staged. Apply settings to save it.");
            return 0;
        case id_settings_apply:
            if (!link_port_valid(*state)) {
                show_link_port_validation(*state);
                SetFocus(state->link_remote_port);
                return 0;
            }
            finish(*state, settings_return_action(state->can_resume));
            return 0;
        case id_settings_cancel:
            cancel_settings(*state, settings_return_action(state->can_resume));
            return 0;
        case id_artwork_retry:
            if (HIWORD(wparam) == BN_CLICKED) {
                start_artwork_resolution(*state);
            }
            return 0;
        default: break;
        }
    } else if (message == WM_MEASUREITEM) {
        const auto* measure = reinterpret_cast<const MEASUREITEMSTRUCT*>(lparam);
        if (measure != nullptr && measure->CtlType == ODT_COMBOBOX) {
            auto* mutable_measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            mutable_measure->itemHeight = 26;
            return TRUE;
        }
    } else if (message == WM_DRAWITEM) {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item.CtlType == ODT_COMBOBOX) {
            draw_dashboard_combo(item);
        } else if (GetPropW(item.hwndItem, L"GBB_DASHBOARD_CHECKBOX") !=
                   nullptr) {
            draw_dashboard_checkbox(item);
        } else if (wparam == id_voxel_preview) {
            draw_voxel_preview(item, *state);
        } else if (wparam == id_gameboy_background) {
            draw_gameboy_background(item);
        } else {
            draw_dashboard_button(item, *state);
        }
        return TRUE;
    } else if (message == WM_NOTIFY) {
        const auto* notification = reinterpret_cast<NMHDR*>(lparam);
        if (notification->code == NM_CUSTOMDRAW) {
            const auto header = ListView_GetHeader(state->list);
            if (notification->hwndFrom == header) {
                const auto* custom = reinterpret_cast<const NMCUSTOMDRAW*>(lparam);
                if (custom->dwDrawStage == CDDS_PREPAINT) {
                    return CDRF_NOTIFYITEMDRAW;
                }
                if (custom->dwDrawStage == CDDS_ITEMPREPAINT) {
                    RECT rectangle{};
                    const auto column = static_cast<int>(custom->dwItemSpec);
                    if (!Header_GetItemRect(header, column, &rectangle)) {
                        return CDRF_DODEFAULT;
                    }
                    const auto background = CreateSolidBrush(RGB(20, 30, 44));
                    FillRect(custom->hdc, &rectangle, background);
                    DeleteObject(background);
                    wchar_t label[128]{};
                    HDITEMW item{};
                    item.mask = HDI_TEXT;
                    item.pszText = label;
                    item.cchTextMax = static_cast<int>(std::size(label));
                    if (!SendMessageW(header, HDM_GETITEMW,
                                      static_cast<WPARAM>(column),
                                      reinterpret_cast<LPARAM>(&item))) {
                        return CDRF_DODEFAULT;
                    }
                    SetBkMode(custom->hdc, TRANSPARENT);
                    SetTextColor(custom->hdc, RGB(185, 216, 232));
                    rectangle.left += 12;
                    DrawTextW(custom->hdc, label, -1, &rectangle,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    return CDRF_SKIPDEFAULT;
                }
            } else if (notification->idFrom == id_list) {
                auto* custom = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
                if (custom->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (custom->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    const auto row = static_cast<int>(custom->nmcd.dwItemSpec);
                    const auto selected =
                        (ListView_GetItemState(state->list, row, LVIS_SELECTED) &
                         LVIS_SELECTED) != 0;
                    custom->clrText = RGB(238, 246, 252);
                    custom->clrTextBk = selected ? RGB(0, 104, 141)
                                                  : RGB(20, 27, 38);
                    return CDRF_NEWFONT;
                }
            }
        }
        if (notification->idFrom == id_list &&
            notification->code == LVN_ITEMCHANGED) {
            refresh_library_actions(*state);
        }
        if (notification->idFrom == id_list &&
            notification->code == LVN_COLUMNCLICK) {
            const auto* column = reinterpret_cast<const NMLISTVIEW*>(lparam);
            if (column != nullptr && column->iSubItem > 0 &&
                column->iSubItem < 5) {
                if (state->library_sort_column == column->iSubItem) {
                    state->library_sort_descending =
                        !state->library_sort_descending;
                } else {
                    state->library_sort_column = column->iSubItem;
                    state->library_sort_descending = column->iSubItem == 4;
                }
                refresh_library_list(*state);
                InvalidateRect(ListView_GetHeader(state->list), nullptr, TRUE);
                return 0;
            }
        }
        if (notification->idFrom == id_list &&
            notification->code == LVN_KEYDOWN) {
            const auto* key = reinterpret_cast<const NMLVKEYDOWN*>(lparam);
            if (key->wVKey == VK_RETURN) {
                play_selection(*state);
                return 0;
            }
        }
        if (notification->idFrom == id_list && notification->code == NM_DBLCLK) {
            play_selection(*state);
            return 0;
        }
    } else if (message == artwork_ready) {
        const std::unique_ptr<ArtworkUpdate> update(
            reinterpret_cast<ArtworkUpdate*>(lparam));
        if (update->index >= state->library->entries().size()) return 0;
        LVFINDINFOW find{};
        find.flags = LVFI_PARAM;
        find.lParam = static_cast<LPARAM>(update->index);
        const auto row = static_cast<int>(SendMessageW(
            state->list, LVM_FINDITEMW, static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>(&find)));
        if (row < 0) return 0;
        LVITEMW item{};
        auto title = widen(update->title);
        item.iSubItem = 1;
        item.pszText = title.data();
        SendMessageW(state->list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(row),
                     reinterpret_cast<LPARAM>(&item));
        auto language = widen(update->language);
        item.iSubItem = 3;
        item.pszText = language.data();
        SendMessageW(state->list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(row),
                     reinterpret_cast<LPARAM>(&item));
        if (!update->cover.empty()) {
            if (auto bitmap = load_file_bitmap(update->cover, 48, 66)) {
                const auto image = ImageList_Add(state->covers, bitmap, nullptr);
                DeleteObject(bitmap);
                if (image >= 0) {
                    LVITEMW image_item{};
                    image_item.mask = LVIF_IMAGE;
                    image_item.iItem = row;
                    image_item.iImage = image;
                    SendMessageW(state->list, LVM_SETITEMW, 0,
                                 reinterpret_cast<LPARAM>(&image_item));
                }
            }
        }
        return 0;
    } else if (message == WM_CLOSE) {
        if (state->settings_dirty) {
            if (MessageBoxW(
                    window,
                    L"Discard your unsaved settings changes?",
                    L"Unsaved settings",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                // Closing the dashboard is an application-exit request. Do
                // not use the settings return action here: when a game is
                // active that action is resume, which would reopen the
                // dashboard from the runtime after the user confirmed exit.
                cancel_settings(*state, DashboardResultAction::quit);
            }
        } else if (confirm_exit(window)) {
            finish(*state, DashboardResultAction::quit);
        }
        return 0;
    } else if (message == WM_DESTROY) {
        KillTimer(window, 1);
        if (state->logo_bitmap != nullptr) {
            DeleteObject(state->logo_bitmap);
            state->logo_bitmap = nullptr;
        }
        if (state->covers != nullptr) {
            ImageList_Destroy(state->covers);
            state->covers = nullptr;
        }
        if (state->background_brush != nullptr) {
            DeleteObject(state->background_brush);
            state->background_brush = nullptr;
        }
        if (state->ui_font != nullptr) {
            DeleteObject(state->ui_font);
            state->ui_font = nullptr;
        }
        if (state->title_font != nullptr) {
            DeleteObject(state->title_font);
            state->title_font = nullptr;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HBITMAP load_logo_bitmap(HINSTANCE instance, const UINT width,
                         const UINT height) {
    const auto resource = FindResourceW(
        instance, MAKEINTRESOURCEW(IDR_GBB_LOGO), MAKEINTRESOURCEW(10));
    if (resource == nullptr) return nullptr;
    const auto loaded = LoadResource(instance, resource);
    auto* bytes = static_cast<BYTE*>(LockResource(loaded));
    const auto byte_count = SizeofResource(instance, resource);
    if (bytes == nullptr || byte_count == 0) return nullptr;

    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* factory{};
    IWICStream* stream{};
    IWICBitmapDecoder* decoder{};
    IWICBitmapFrameDecode* frame{};
    IWICBitmapScaler* scaler{};
    IWICFormatConverter* converter{};
    HBITMAP bitmap{};
    if (SUCCEEDED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(bytes, byte_count)) &&
        SUCCEEDED(factory->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(frame, width, height,
                                    WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(
            scaler, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0,
            WICBitmapPaletteTypeCustom))) {
        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        information.bmiHeader.biWidth = static_cast<LONG>(width);
        information.bmiHeader.biHeight = -static_cast<LONG>(height);
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        information.bmiHeader.biCompression = BI_RGB;
        void* pixels{};
        bitmap = CreateDIBSection(nullptr, &information, DIB_RGB_COLORS,
                                  &pixels, nullptr, 0);
        if (bitmap == nullptr || FAILED(converter->CopyPixels(
                nullptr, width * 4, width * height * 4,
                static_cast<BYTE*>(pixels)))) {
            if (bitmap != nullptr) DeleteObject(bitmap);
            bitmap = nullptr;
        }
    }
    if (converter != nullptr) converter->Release();
    if (scaler != nullptr) scaler->Release();
    if (frame != nullptr) frame->Release();
    if (decoder != nullptr) decoder->Release();
    if (stream != nullptr) stream->Release();
    if (factory != nullptr) factory->Release();
    if (SUCCEEDED(initialized)) CoUninitialize();
    return bitmap;
}

HBITMAP load_file_bitmap(const std::filesystem::path& path, const UINT width,
                         const UINT height) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* factory{};
    IWICBitmapDecoder* decoder{};
    IWICBitmapFrameDecode* frame{};
    IWICBitmapScaler* scaler{};
    IWICFormatConverter* converter{};
    HBITMAP bitmap{};
    const auto filename = widen(path.u8string());
    if (SUCCEEDED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(
            filename.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(frame, width, height,
                                    WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(
            scaler, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0,
            WICBitmapPaletteTypeCustom))) {
        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        information.bmiHeader.biWidth = static_cast<LONG>(width);
        information.bmiHeader.biHeight = -static_cast<LONG>(height);
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        information.bmiHeader.biCompression = BI_RGB;
        void* pixels{};
        bitmap = CreateDIBSection(nullptr, &information, DIB_RGB_COLORS,
                                  &pixels, nullptr, 0);
        if (bitmap == nullptr || FAILED(converter->CopyPixels(
                nullptr, width * 4, width * height * 4,
                static_cast<BYTE*>(pixels)))) {
            if (bitmap != nullptr) DeleteObject(bitmap);
            bitmap = nullptr;
        }
    }
    if (converter != nullptr) converter->Release();
    if (scaler != nullptr) scaler->Release();
    if (frame != nullptr) frame->Release();
    if (decoder != nullptr) decoder->Release();
    if (factory != nullptr) factory->Release();
    if (SUCCEEDED(initialized)) CoUninitialize();
    return bitmap;
}

void resolve_artwork(State& state) {
    std::unordered_map<std::string,
        std::unordered_map<std::uint32_t, artwork::MetadataRecord>> databases;
    for (std::size_t index = 0; index < state.library->entries().size(); ++index) {
        if (state.closing) return;
        const auto& entry = state.library->entries()[index];
        auto metadata = entry.metadata;
        try {
            metadata = gameboy::inspect_rom_file(entry.path);
        } catch (const std::exception&) {
        }
        const std::string system = gameboy::cover_system_name(metadata.platform);
        auto found_database = databases.find(system);
        if (found_database == databases.end()) {
            found_database = databases.emplace(
                system, artwork::load_database(state.preference_directory, system,
                                                &state.artwork_download)).first;
        }
        auto title = metadata.title;
        auto language = metadata.language;
        auto canonical = metadata.cover_name;
        if (const auto record = found_database->second.find(metadata.crc32);
            record != found_database->second.end()) {
            canonical = record->second.name;
            title = artwork::display_title(canonical);
            language = record->second.language;
        }

        std::ostringstream fingerprint;
        fingerprint << std::hex << std::setw(16) << std::setfill('0')
                    << metadata.fingerprint;
        const auto cover = state.preference_directory / "covers" /
                           (fingerprint.str() + ".png");
        if (!std::filesystem::is_regular_file(cover)) {
            std::string error;
            const auto url = "https://thumbnails.libretro.com/" +
                artwork::url_component(system) + "/Named_Boxarts/" +
                artwork::url_component(artwork::thumbnail_name(canonical)) + ".png";
            static_cast<void>(download_public_file(
                url, cover, 5 * 1024 * 1024, error, &state.artwork_download));
        }
        if (!std::filesystem::is_regular_file(cover)) {
            state.artwork_failed.fetch_add(1, std::memory_order_relaxed);
        }
        auto update = std::make_unique<ArtworkUpdate>(ArtworkUpdate{
            index, std::move(title), std::move(language),
            std::filesystem::is_regular_file(cover)
                ? cover
                : std::filesystem::path{}});
        state.artwork_completed.fetch_add(1, std::memory_order_relaxed);
        if (!PostMessageW(state.window, artwork_ready, 0,
                          reinterpret_cast<LPARAM>(update.get()))) {
            return;
        }
        static_cast<void>(update.release());
    }
}

void update_artwork_retry_visibility(State& state) {
    if (state.artwork_retry == nullptr) return;
    const auto ready = state.artwork_completed.load(std::memory_order_relaxed) >=
                       state.artwork_total;
    const auto failed = state.artwork_failed.load(std::memory_order_relaxed) != 0;
    ShowWindow(state.artwork_retry,
               state.page == State::Page::library && ready && failed
                   ? SW_SHOW
                   : SW_HIDE);
}

void start_artwork_resolution(State& state) {
    if (state.artwork_worker.joinable()) state.artwork_worker.join();
    state.artwork_completed = 0;
    state.artwork_failed = 0;
    state.artwork_download.cancel_requested.store(false,
                                                   std::memory_order_relaxed);
    state.artwork_total = state.library->entries().size();
    SetWindowTextW(state.artwork_status,
                   state.artwork_total == 0 ? L"Artwork: ready"
                                            : L"Artwork: loading...");
    if (state.artwork_total == 0) {
        KillTimer(state.window, 1);
    } else {
        SetTimer(state.window, 1, 100, nullptr);
        state.artwork_worker = std::thread([&state] { resolve_artwork(state); });
    }
    update_artwork_retry_visibility(state);
}

HWND control(State& state, const wchar_t* type, const wchar_t* text,
             DWORD style, int x, int y, int width, int height, int id) {
    const auto control_type = std::wstring_view(type);
    const auto checkbox = control_type == L"BUTTON" &&
                          (style & BS_TYPEMASK) == BS_AUTOCHECKBOX;
    if (control_type == L"BUTTON") style |= BS_OWNERDRAW;
    if (control_type == L"COMBOBOX") {
        style |= CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
    }
    const auto extended_style = control_type == L"STATIC" ? WS_EX_TRANSPARENT : 0;
    // Win32 does not add tab stops to owner-drawn buttons automatically.
    // Keep the complete dashboard keyboard-accessible.
    if (control_type == L"BUTTON" || control_type == L"COMBOBOX" ||
        control_type == L"EDIT" || control_type == WC_LISTVIEWW) {
        style |= WS_TABSTOP;
    }
    auto result = CreateWindowExW(extended_style, type, text,
        WS_CHILD | style, x, y, width, height, state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (checkbox && result != nullptr) {
        // BS_OWNERDRAW replaces the button type bits, so preserve the
        // semantic type explicitly for WM_DRAWITEM.
        SetPropW(result, L"GBB_DASHBOARD_CHECKBOX",
                 reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
        set_dashboard_checkbox_checked(result, false);
    }
    const auto font = state.ui_font != nullptr
                          ? state.ui_font
                          : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return result;
}

bool confirm_exit(HWND window) {
    return MessageBoxW(
               window,
               L"Are you sure you want to close Go Bigger Boy?\n\n"
               L"Any game currently running will be stopped.",
               L"Exit Go Bigger Boy?", MB_YESNO | MB_ICONQUESTION |
                   MB_DEFBUTTON2) == IDYES;
}

void draw_dashboard_button(const DRAWITEMSTRUCT& item, const State& state) {
    const auto id = GetDlgCtrlID(item.hwndItem);
    const auto pressed = (item.itemState & ODS_SELECTED) != 0;
    const auto disabled = (item.itemState & ODS_DISABLED) != 0;
    const auto active_tab =
        (id == id_settings && state.page == State::Page::settings) ||
        (id == id_library && state.page == State::Page::library) ||
        (id == id_shortcuts && state.page == State::Page::shortcuts);
    const auto section = id >= id_settings_section_first &&
                         id < id_settings_section_first + 4;
    const auto active_section =
        section && state.page == State::Page::settings &&
        id - id_settings_section_first ==
            static_cast<int>(state.settings_section);
    const auto fill = disabled
                          ? RGB(28, 34, 45)
                          : active_tab || active_section || pressed
                                ? RGB(0, 145, 190)
                                                   : RGB(44, 65, 88);
    const auto text = disabled ? RGB(104, 117, 132) : RGB(238, 246, 252);
    const auto brush = CreateSolidBrush(fill);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, text);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(
        item.hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = nullptr;
    if (font != nullptr) old_font = SelectObject(item.hDC, font);
    wchar_t label[256]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    auto text_rect = item.rcItem;
    DrawTextW(item.hDC, label, -1, &text_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if ((item.itemState & ODS_FOCUS) != 0) {
        auto focus_rect = item.rcItem;
        InflateRect(&focus_rect, -3, -3);
        const auto focus_pen = CreatePen(PS_DOT, 1, RGB(238, 246, 252));
        const auto old_pen = SelectObject(item.hDC, focus_pen);
        const auto old_brush = SelectObject(item.hDC, GetStockObject(HOLLOW_BRUSH));
        Rectangle(item.hDC, focus_rect.left, focus_rect.top,
                  focus_rect.right, focus_rect.bottom);
        SelectObject(item.hDC, old_brush);
        SelectObject(item.hDC, old_pen);
        DeleteObject(focus_pen);
    }
    if (old_font != nullptr) SelectObject(item.hDC, old_font);
}

void draw_dashboard_checkbox(const DRAWITEMSTRUCT& item) {
    const auto disabled = (item.itemState & ODS_DISABLED) != 0;
    const auto pressed = (item.itemState & ODS_SELECTED) != 0;
    const auto checked = dashboard_checkbox_checked(item.hwndItem);
    const auto background = CreateSolidBrush(
        pressed ? RGB(20, 77, 101) : RGB(13, 18, 27));
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);

    RECT box{item.rcItem.left + 2,
             item.rcItem.top + (item.rcItem.bottom - item.rcItem.top - 18) / 2,
             item.rcItem.left + 20,
             item.rcItem.top + (item.rcItem.bottom - item.rcItem.top - 18) / 2 +
                 18};
    const auto box_fill = CreateSolidBrush(
        disabled ? RGB(28, 34, 45) : checked ? RGB(0, 104, 141)
                                            : RGB(20, 27, 38));
    FillRect(item.hDC, &box, box_fill);
    DeleteObject(box_fill);
    const auto box_border = CreateSolidBrush(
        disabled ? RGB(104, 117, 132) : RGB(69, 207, 238));
    FrameRect(item.hDC, &box, box_border);
    DeleteObject(box_border);

    if (checked && !disabled) {
        const auto check_pen = CreatePen(PS_SOLID, 2, RGB(238, 249, 255));
        const auto old_pen = SelectObject(item.hDC, check_pen);
        MoveToEx(item.hDC, box.left + 4, box.top + 9, nullptr);
        LineTo(item.hDC, box.left + 8, box.bottom - 4);
        LineTo(item.hDC, box.right - 3, box.top + 4);
        SelectObject(item.hDC, old_pen);
        DeleteObject(check_pen);
    }

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(104, 117, 132)
                                    : RGB(238, 246, 252));
    const auto font = reinterpret_cast<HFONT>(SendMessageW(
        item.hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = nullptr;
    if (font != nullptr) old_font = SelectObject(item.hDC, font);
    wchar_t label[256]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    auto text_rect = item.rcItem;
    text_rect.left = box.right + 8;
    DrawTextW(item.hDC, label, -1, &text_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                  DT_END_ELLIPSIS);
    if ((item.itemState & ODS_FOCUS) != 0) {
        auto focus_rect = item.rcItem;
        InflateRect(&focus_rect, -1, -1);
        const auto focus_pen = CreatePen(PS_DOT, 1, RGB(238, 246, 252));
        const auto old_pen = SelectObject(item.hDC, focus_pen);
        const auto old_brush = SelectObject(item.hDC, GetStockObject(HOLLOW_BRUSH));
        Rectangle(item.hDC, focus_rect.left, focus_rect.top,
                  focus_rect.right, focus_rect.bottom);
        SelectObject(item.hDC, old_brush);
        SelectObject(item.hDC, old_pen);
        DeleteObject(focus_pen);
    }
    if (old_font != nullptr) SelectObject(item.hDC, old_font);
}

void draw_dashboard_combo(const DRAWITEMSTRUCT& item) {
    const auto disabled = (item.itemState & ODS_DISABLED) != 0;
    const auto closed = item.itemID == static_cast<UINT>(-1);
    const auto current = SendMessageW(item.hwndItem, CB_GETCURSEL, 0, 0);
    const auto index = closed ? current : static_cast<LRESULT>(item.itemID);
    const auto selected = !closed &&
                          (item.itemState & ODS_SELECTED) != 0;
    const auto background = CreateSolidBrush(
        disabled ? RGB(28, 34, 45)
                 : selected ? RGB(0, 104, 141) : RGB(20, 27, 38));
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);
    const auto border = CreateSolidBrush(
        disabled ? RGB(104, 117, 132) : RGB(69, 207, 238));
    FrameRect(item.hDC, &item.rcItem, border);
    DeleteObject(border);

    wchar_t label[256]{};
    if (index >= 0) {
        SendMessageW(item.hwndItem, CB_GETLBTEXT,
                     static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(label));
    }
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(104, 117, 132)
                                    : RGB(238, 246, 252));
    const auto font = reinterpret_cast<HFONT>(SendMessageW(
        item.hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = nullptr;
    if (font != nullptr) old_font = SelectObject(item.hDC, font);
    auto text_rect = item.rcItem;
    text_rect.left += 9;
    text_rect.right -= 24;
    DrawTextW(item.hDC, label, -1, &text_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                  DT_END_ELLIPSIS);
    if (closed) {
        const auto arrow_brush = CreateSolidBrush(
            disabled ? RGB(104, 117, 132) : RGB(185, 216, 232));
        const auto old_brush = SelectObject(item.hDC, arrow_brush);
        const auto center_x = item.rcItem.right - 13;
        const auto center_y = (item.rcItem.top + item.rcItem.bottom) / 2;
        const std::array<POINT, 3> arrow{
            POINT{center_x - 5, center_y - 2},
            POINT{center_x + 5, center_y - 2},
            POINT{center_x, center_y + 4}};
        Polygon(item.hDC, arrow.data(), static_cast<int>(arrow.size()));
        SelectObject(item.hDC, old_brush);
        DeleteObject(arrow_brush);
    }
    if ((item.itemState & ODS_FOCUS) != 0) {
        auto focus_rect = item.rcItem;
        InflateRect(&focus_rect, -1, -1);
        const auto focus_pen = CreatePen(PS_DOT, 1, RGB(238, 246, 252));
        const auto old_pen = SelectObject(item.hDC, focus_pen);
        const auto old_brush = SelectObject(item.hDC, GetStockObject(HOLLOW_BRUSH));
        Rectangle(item.hDC, focus_rect.left, focus_rect.top,
                  focus_rect.right, focus_rect.bottom);
        SelectObject(item.hDC, old_brush);
        SelectObject(item.hDC, old_pen);
        DeleteObject(focus_pen);
    }
    if (old_font != nullptr) SelectObject(item.hDC, old_font);
}

} // namespace

DashboardResult show_windows_dashboard(
    HWND owner, const gameboy::RomLibrary& library, const bool can_resume,
    const std::uint64_t current_fingerprint,
    const gbb::CoreCapability capabilities,
    const std::size_t palette, const gameboy::VideoMode video_mode,
    const gameboy::HardwareModel hardware_model,
    const bool audio_enabled,
    const KeyboardBindings& keyboard_bindings,
    const ActionBindings& action_bindings,
    const DashboardLinkSettings& link_settings,
    const gbb::PluginDiscoveryOptions& plugin_options,
    const gbb::PluginCatalog& plugin_catalog,
    const std::filesystem::path& preference_directory,
    const std::function<bool()>& poll_update) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    const auto instance = GetModuleHandleW(nullptr);
    constexpr auto class_name = L"GoBiggerBoyDashboard";
    WNDCLASSW type{};
    type.lpfnWndProc = window_proc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_GBB_ICON));
    type.hbrBackground = nullptr;
    type.lpszClassName = class_name;
    RegisterClassW(&type);

    State state;
    state.background_brush = CreateSolidBrush(RGB(13, 18, 27));
    state.ui_font = CreateFontW(
        -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    state.title_font = CreateFontW(
        -20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    state.library = &library;
    state.can_resume = can_resume;
    state.voxel_available =
        gbb::has_capability(capabilities, gbb::CoreCapability::scene_layers);
    state.voxel_fingerprint = current_fingerprint;
    state.voxel_profile_path = preference_directory.empty()
                                   ? std::filesystem::path{}
                                   : preference_directory / "voxel-profiles.ini";
    state.voxel_profile = gbb::load_voxel_profile(state.voxel_profile_path,
                                                   current_fingerprint);
    state.initial_voxel_profile = state.voxel_profile;
    state.result.palette = palette;
    state.result.video_mode = video_mode;
    state.result.hardware_model = hardware_model;
    state.result.audio_enabled = audio_enabled;
    state.result.keyboard_bindings = keyboard_bindings;
    state.result.action_bindings = action_bindings;
    state.result.link_settings = link_settings;
    state.initial_link_settings = link_settings;
    state.result.plugin_discovery = plugin_options.enabled;
    state.result.plugin_require_allowlist = plugin_options.require_allowlist;
    state.result.plugin_require_capability_allowlist =
        plugin_options.require_capability_allowlist;
    state.plugin_options = plugin_options;
    state.plugin_status_text = plugin_status_text(plugin_options, plugin_catalog);
    state.initial_result = state.result;
    state.settings_dirty = false;
    state.preference_directory = preference_directory;
    state.poll_update = poll_update;
    const auto saved_position = load_window_position(preference_directory);
    RECT work_area{};
    const auto work_area_available = SystemParametersInfoW(
        SPI_GETWORKAREA, 0, &work_area, 0) != FALSE;
    const auto work_height = work_area_available
                                 ? work_area.bottom - work_area.top
                                 : 0L;
    const auto initial_height = std::max(
        520, std::min(dashboard_height, static_cast<int>(
                                  work_height > 80
                                      ? work_height - 40
                                      : dashboard_height)));
    const auto window_title = std::wstring{L"Go Bigger Boy - Game Library v"} +
                              widen(GBB_VERSION);
    state.window = CreateWindowExW(
        WS_EX_APPWINDOW, class_name, window_title.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_THICKFRAME | WS_VSCROLL | WS_CLIPCHILDREN,
        saved_position ? saved_position->x : CW_USEDEFAULT,
        saved_position ? saved_position->y : CW_USEDEFAULT,
        dashboard_width, initial_height, owner, nullptr, instance, &state);
    if (state.window == nullptr) {
        if (state.background_brush != nullptr) {
            DeleteObject(state.background_brush);
            state.background_brush = nullptr;
        }
        if (state.ui_font != nullptr) {
            DeleteObject(state.ui_font);
            state.ui_font = nullptr;
        }
        if (state.title_font != nullptr) {
            DeleteObject(state.title_font);
            state.title_font = nullptr;
        }
        state.result.action = can_resume ? DashboardResultAction::resume
                                         : DashboardResultAction::quit;
        return state.result;
    }

    SendMessageW(state.window, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(type.hIcon));
    SendMessageW(state.window, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(type.hIcon));
    state.logo_bitmap = load_logo_bitmap(instance, 300, 100);
    state.logo = control(state, L"STATIC", L"Go Bigger Boy",
        WS_VISIBLE | (state.logo_bitmap != nullptr ? SS_BITMAP : 0),
        32, 18, 300, 100, 0);
    if (state.logo_bitmap != nullptr) {
        SendMessageW(state.logo, STM_SETIMAGE, IMAGE_BITMAP,
                     reinterpret_cast<LPARAM>(state.logo_bitmap));
    }
    state.library_tab = control(state, L"BUTTON", L"Library",
                                WS_VISIBLE | BS_PUSHBUTTON,
                                32, 140, 126, 40, id_library);
    state.settings_tab = control(state, L"BUTTON", L"Settings",
                                 WS_VISIBLE | BS_PUSHBUTTON,
                                 174, 140, 126, 40, id_settings);
    state.shortcuts_tab = control(state, L"BUTTON", L"Shortcuts",
                                  WS_VISIBLE | BS_PUSHBUTTON,
                                  316, 140, 126, 40, id_shortcuts);
    state.settings_status = control(
        state, L"STATIC", L"Changes are staged until you apply or discard them.",
        WS_VISIBLE, 510, 112, 440, 20, 0);
    state.settings_apply = control(
        state, L"BUTTON", L"Apply and return", BS_DEFPUSHBUTTON,
        650, 140, 140, 40, id_settings_apply);
    state.settings_cancel = control(
        state, L"BUTTON", L"Discard", BS_PUSHBUTTON,
        802, 140, 126, 40, id_settings_cancel);
    state.settings_section_description = control(
        state, L"STATIC",
        L"Choose the display, audio, and hardware behavior used when games run.",
        WS_VISIBLE, 32, 280, 916, 38, 0);
    constexpr std::array<const wchar_t*, 4> settings_section_names{{
        L"General", L"Controls", L"Link cable", L"Advanced"}};
    for (std::size_t index = 0; index < settings_section_names.size(); ++index) {
        state.settings_sections[index] = control(
            state, L"BUTTON", settings_section_names[index], BS_PUSHBUTTON,
            32 + static_cast<int>(index) * 226, 235,
            index == settings_section_names.size() - 1 ? 238 : 210, 34,
            id_settings_section_first + static_cast<int>(index));
    }
    state.artwork_status = control(
        state, L"STATIC", L"Artwork: loading...", WS_VISIBLE,
        32, 180, 916, 20, 0);
    state.artwork_retry = control(
        state, L"BUTTON", L"Retry artwork", BS_PUSHBUTTON,
        850, 176, 98, 28, id_artwork_retry);
    state.list = control(state, WC_LISTVIEWW, L"",
        WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        32, 200, 916, 350, id_list);
    state.library_empty = control(
        state, L"STATIC",
        L"Welcome to Go Bigger Boy.\n\nChoose Open ROM... to add your first game to the library.",
        WS_VISIBLE | SS_CENTER,
        32, 300, 916, 100, 0);
    SetWindowSubclass(ListView_GetHeader(state.list), table_header_subclass, 1,
                      reinterpret_cast<DWORD_PTR>(&state));
    ListView_SetExtendedListViewStyle(state.list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(state.list, RGB(20, 27, 38));
    ListView_SetTextBkColor(state.list, RGB(20, 27, 38));
    ListView_SetTextColor(state.list, RGB(234, 242, 248));
    state.covers = ImageList_Create(48, 66, ILC_COLOR32, 1,
                                    static_cast<int>(library.entries().size()));
    if (state.covers != nullptr) {
        BITMAPINFO placeholder_info{};
        placeholder_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        placeholder_info.bmiHeader.biWidth = 48;
        placeholder_info.bmiHeader.biHeight = -66;
        placeholder_info.bmiHeader.biPlanes = 1;
        placeholder_info.bmiHeader.biBitCount = 32;
        placeholder_info.bmiHeader.biCompression = BI_RGB;
        void* pixels{};
        auto placeholder = CreateDIBSection(nullptr, &placeholder_info,
            DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (placeholder != nullptr) {
            std::fill_n(static_cast<std::uint32_t*>(pixels), 48 * 66,
                        UINT32_C(0xFFE1E4EB));
            ImageList_Add(state.covers, placeholder, nullptr);
            DeleteObject(placeholder);
        }
        ListView_SetImageList(state.list, state.covers, LVSIL_SMALL);
    }
    constexpr std::array<std::pair<const wchar_t*, int>, 6> columns{{
        {L"Cover", 62}, {L"Game", 260}, {L"Platform", 125},
        {L"Language", 125}, {L"Last played", 170}, {L"ROM path", 420}}};
    for (std::size_t index = 0; index < columns.size(); ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(columns[index].first);
        column.cx = columns[index].second;
        SendMessageW(state.list, LVM_INSERTCOLUMNW,
                     static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&column));
    }
    state.search_label = control(state, L"STATIC", L"Filter games", 0,
                                 32, 180, 110, 26, 0);
    state.search = control(state, L"EDIT", L"",
                           WS_BORDER | ES_AUTOHSCROLL | ES_LEFT,
                           154, 176, 400, 28, id_search);
    state.search_summary = control(
        state, L"STATIC", L"0 games", SS_RIGHT,
        570, 180, 250, 26, 0);
    state.search_clear = control(
        state, L"BUTTON", L"Clear search", BS_PUSHBUTTON,
        830, 176, 118, 28, id_search_clear);
    refresh_library_list(state);
    state.open = control(state, L"BUTTON", L"Open ROM...",
        WS_VISIBLE | BS_PUSHBUTTON, 32, 575, 150, 44, id_open);
    state.play = control(state, L"BUTTON", L"Play selected",
        WS_VISIBLE | BS_DEFPUSHBUTTON, 202, 575, 160, 44, id_play);
    state.remove = control(state, L"BUTTON", L"Remove from list",
        WS_VISIBLE | BS_PUSHBUTTON, 382, 575, 170, 44, id_remove);
    state.resume = control(state, L"BUTTON", L"Resume game",
        (can_resume ? WS_VISIBLE : 0) | BS_PUSHBUTTON,
        572, 575, 150, 44, id_resume);
    state.settings_heading = control(state, L"STATIC", L"Settings",
        0, 32, 200, 360, 30, 0);
    SendMessageW(state.settings_heading, WM_SETFONT,
                 reinterpret_cast<WPARAM>(state.title_font), TRUE);
    state.palette_label = control(state, L"STATIC", L"Display palette",
        0, 32, 245, 110, 26, 0);
    state.palette = control(state, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL, 154, 240, 290, 200, id_palette);
    constexpr std::array<const wchar_t*, 5> palettes{{
        L"Grayscale", L"Classic green", L"Game Boy Pocket", L"Amber",
        L"Game Boy Color (automatic)"}};
    for (const auto* name : palettes) {
        SendMessageW(state.palette, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name));
    }
    SendMessageW(state.palette, CB_SETCURSEL,
                 static_cast<WPARAM>(palette), 0);
    state.video_label = control(state, L"STATIC", L"Video mode",
        0, 32, 275, 110, 26, 0);
    state.video = control(state, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL, 154, 270, 290, 26, id_video);
    for (const auto& info : gameboy::video_modes) {
        const auto name = widen(std::string{info.name});
        SendMessageW(state.video, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name.c_str()));
    }
    // The closed combo box only needs a single-row client height. Tell
    // Windows how many rows to show in the popup instead of using a large
    // control rectangle that would cover the controller artwork.
    SendMessageW(state.video, CB_SETMINVISIBLE,
                 static_cast<WPARAM>(gameboy::video_modes.size()), 0);
    const auto selected_video = std::distance(
        gameboy::video_modes.begin(),
        std::find_if(gameboy::video_modes.begin(), gameboy::video_modes.end(),
                     [video_mode](const auto& info) {
                         return info.mode == video_mode;
                     }));
    SendMessageW(state.video, CB_SETCURSEL,
                 static_cast<WPARAM>(selected_video), 0);
    state.hardware_model_label = control(state, L"STATIC", L"Hardware model",
        0, 32, 300, 110, 26, 0);
    state.hardware_model = control(state, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL, 154, 300, 290, 200,
        id_hardware_model);
    for (const auto model : gameboy::selectable_hardware_models) {
        const auto name = widen(std::string{gameboy::hardware_model_name(model)});
        SendMessageW(state.hardware_model, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name.c_str()));
    }
    auto selected_model = std::distance(
        gameboy::selectable_hardware_models.begin(),
        std::find(gameboy::selectable_hardware_models.begin(),
                  gameboy::selectable_hardware_models.end(), hardware_model));
    if (selected_model < 0) selected_model = 0;
    SendMessageW(state.hardware_model, CB_SETCURSEL,
                 static_cast<WPARAM>(selected_model), 0);
    state.audio_enabled = control(
        state, L"BUTTON", L"Generate audio",
        WS_TABSTOP | BS_AUTOCHECKBOX, 510, 270, 300, 34, id_audio_enabled);
    set_dashboard_checkbox_checked(state.audio_enabled, audio_enabled);
    state.controls_label = control(state, L"STATIC", L"Keyboard controls",
        0, 510, 200, 240, 30, 0);
    SendMessageW(state.controls_label, WM_SETFONT,
                 reinterpret_cast<WPARAM>(state.title_font), TRUE);
    state.controls_instruction = control(
        state, L"STATIC",
        L"Click a slot, press a key, or press Delete to clear it. Duplicate keys are moved from their previous action.",
        0, 510, 238, 420, 26, 0);
    state.gameboy_background = control(
        state, L"STATIC", L"", SS_OWNERDRAW | WS_CLIPSIBLINGS,
        32, 310, 916, 268,
        id_gameboy_background);

    for (int column = 0; column < 2; ++column) {
        const auto base_x = column == 0 ? 72 : 520;
        state.primary_headings[static_cast<std::size_t>(column)] = control(
            state, L"STATIC", L"Primary key", 0,
            base_x + 78, 316, 120, 24, 0);
        state.secondary_headings[static_cast<std::size_t>(column)] = control(
            state, L"STATIC", L"Secondary key", 0,
            base_x + 214, 316, 120, 24, 0);
    }
    constexpr std::array<std::size_t, 8> control_order{{
        2, 1, 0, 3, 4, 5, 6, 7}};
    for (std::size_t position = 0; position < control_order.size(); ++position) {
        const auto index = control_order[position];
        const auto column = position < 4 ? 0 : 1;
        const auto row = static_cast<int>(position % 4);
        const auto base_x = column == 0 ? 72 : 520;
        const auto y = 350 + row * 48;
        state.binding_labels[index] = control(
            state, L"STATIC", control_names[index], 0,
            base_x, y + 8, 64, 26, 0);
        for (std::size_t slot = 0; slot < 2; ++slot) {
            state.binding_buttons[index][slot] = control(
                state, L"BUTTON", L"", BS_PUSHBUTTON,
                base_x + 78 + static_cast<int>(slot) * 136, y,
                120, 38,
                id_binding_first + static_cast<int>(index * 2 + slot));
        }
    }
    state.actions_label = control(state, L"STATIC", L"Emulator shortcuts",
                                  0, 32, 610, 260, 28, 0);
    for (std::size_t index = 0; index < action_names.size(); ++index) {
        const auto column = index % 2;
        const auto row = index / 2;
        const auto base_x = column == 0 ? 72 : 520;
        const auto y = 650 + static_cast<int>(row) * 50;
        state.action_labels[index] = control(
            state, L"STATIC", action_names[index], 0,
            base_x, y + 8, 130, 26, 0);
        state.action_buttons[index] = control(
            state, L"BUTTON", L"", BS_PUSHBUTTON,
            base_x + 145, y, 150, 38,
            id_action_first + static_cast<int>(index));
    }
    state.reset_controls = control(state, L"BUTTON", L"Reset all controls",
        BS_PUSHBUTTON, 32, 775, 230, 40, id_reset_controls);
    state.voxel_heading = control(state, L"STATIC", L"Voxel diorama profile",
                                  0, 32, 825, 320, 28, 0);
    SendMessageW(state.voxel_heading, WM_SETFONT,
                 reinterpret_cast<WPARAM>(state.title_font), TRUE);
    state.voxel_fingerprint_label = control(
        state, L"STATIC", L"", 0, 360, 827, 580, 24, 0);
    state.voxel_preview = control(
        state, L"STATIC", L"", SS_OWNERDRAW | WS_BORDER,
        300, 850, 180, 150, id_voxel_preview);
    const auto profile_columns = std::array<int, 15>{{
        32, 32, 32, 32, 32, 32, 32,
        510, 510, 510, 510, 510, 510, 510, 510}};
    const auto profile_rows = std::array<int, 15>{{
        850, 890, 930, 970, 1010, 1050, 1090,
        850, 890, 930, 970, 1010, 1050, 1090, 1130}};
    for (std::size_t index = 0; index < voxel_profile_names.size(); ++index) {
        const auto x = profile_columns[index];
        const auto y = profile_rows[index];
        state.voxel_labels[index] = control(
            state, L"STATIC", voxel_profile_names[index], 0,
            x, y, 120, 24, 0);
        state.voxel_edits[index] = index == 14
            ? control(state, L"BUTTON", L"Enabled", BS_AUTOCHECKBOX,
                      x + 130, y - 2, 130, 28,
                      id_voxel_first_edit + static_cast<int>(index))
            : control(state, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                      x + 130, y - 2, 130, 28,
                      id_voxel_first_edit + static_cast<int>(index));
    }
    state.voxel_save = control(state, L"BUTTON", L"Stage profile",
                               BS_PUSHBUTTON, 680, 1170, 120, 38,
                               id_voxel_save);
    state.voxel_reset = control(state, L"BUTTON", L"Reset to defaults",
                                BS_PUSHBUTTON, 810, 1170, 138, 38,
                                id_voxel_reset);
    state.plugin_heading = control(state, L"STATIC", L"Native plug-ins",
                                   0, 32, 1080, 320, 28, 0);
    SendMessageW(state.plugin_heading, WM_SETFONT,
                 reinterpret_cast<WPARAM>(state.title_font), TRUE);
    state.plugin_status = control(
        state, L"EDIT", state.plugin_status_text.c_str(),
        ES_MULTILINE | ES_READONLY | WS_BORDER | WS_VSCROLL,
        32, 1115, 916, 72, 0);
    state.plugin_discovery = control(
        state, L"BUTTON", L"Enable native plug-in discovery",
        BS_AUTOCHECKBOX, 32, 1195, 260, 28, id_plugin_discovery);
    state.plugin_require_allowlist = control(
        state, L"BUTTON", L"Require core identity allowlist",
        BS_AUTOCHECKBOX, 320, 1195, 320, 28,
        id_plugin_require_allowlist);
    state.plugin_require_capability_allowlist = control(
        state, L"BUTTON", L"Require capability allowlist", BS_AUTOCHECKBOX,
        660, 1195, 290, 28, id_plugin_require_capability_allowlist);
    set_dashboard_checkbox_checked(state.plugin_discovery,
                                   plugin_options.enabled);
    set_dashboard_checkbox_checked(state.plugin_require_allowlist,
                                   plugin_options.require_allowlist);
    set_dashboard_checkbox_checked(
        state.plugin_require_capability_allowlist,
        plugin_options.require_capability_allowlist);
    state.link_heading = control(state, L"STATIC", L"Remote link cable",
                                 0, 32, 1270, 420, 28, 0);
    SendMessageW(state.link_heading, WM_SETFONT,
                 reinterpret_cast<WPARAM>(state.title_font), TRUE);
    state.link_transport_label = control(
        state, L"STATIC", L"Transport", 0, 32, 1310, 120, 26, 0);
    state.link_transport = control(
        state, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
        154, 1305, 220, 26, id_link_transport);
    SendMessageW(state.link_transport, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"TCP (LAN)"));
    SendMessageW(state.link_transport, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Bluetooth Classic"));
    SendMessageW(state.link_transport, CB_SETCURSEL,
                 link_settings.transport == "bluetooth" ? 1 : 0, 0);
    state.link_remote_host_label = control(
        state, L"STATIC", L"Remote host", 0, 400, 1310, 120, 26, 0);
    state.link_remote_host = control(
        state, L"EDIT", widen(link_settings.remote_host).c_str(),
        WS_BORDER | ES_AUTOHSCROLL, 522, 1305, 190, 26,
        id_link_remote_host);
    state.link_remote_bind_label = control(
        state, L"STATIC", L"Bind", 0, 730, 1310, 100, 26, 0);
    state.link_remote_bind = control(
        state, L"EDIT", widen(link_settings.remote_bind).c_str(),
        WS_BORDER | ES_AUTOHSCROLL, 832, 1305, 116, 26,
        id_link_remote_bind);
    state.link_remote_port_label = control(
        state, L"STATIC", L"Port", 0, 32, 1350, 120, 26, 0);
    state.link_remote_port = control(
        state, L"EDIT", std::to_wstring(link_settings.remote_port).c_str(),
        WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 154, 1345, 100, 26,
        id_link_remote_port);
    state.link_lan_discovery = control(
        state, L"BUTTON", L"Advertise for LAN discovery", BS_AUTOCHECKBOX,
        280, 1345, 250, 28, id_link_lan_discovery);
    set_dashboard_checkbox_checked(state.link_lan_discovery,
                                   link_settings.lan_discovery);
    state.link_bluetooth_address_label = control(
        state, L"STATIC", L"Bluetooth address", 0, 32, 1390, 180, 26, 0);
    state.link_bluetooth_address = control(
        state, L"EDIT", widen(link_settings.bluetooth_address).c_str(),
        WS_BORDER | ES_AUTOHSCROLL, 218, 1385, 220, 26,
        id_link_bluetooth_address);
    state.link_bluetooth_choose = control(
        state, L"BUTTON", L"Choose paired device", BS_PUSHBUTTON,
        218, 1418, 220, 28, id_link_bluetooth_choose);
    state.link_bluetooth_uuid_label = control(
        state, L"STATIC", L"Service UUID", 0, 460, 1390, 180, 26, 0);
    state.link_bluetooth_uuid = control(
        state, L"EDIT", widen(link_settings.bluetooth_service_uuid).c_str(),
        WS_BORDER | ES_AUTOHSCROLL, 646, 1385, 302, 26,
        id_link_bluetooth_uuid);
    state.link_diagnostics = control(
        state, L"BUTTON", L"Write link diagnostics trace", BS_AUTOCHECKBOX,
        32, 1460, 330, 28, id_link_diagnostics);
    set_dashboard_checkbox_checked(state.link_diagnostics,
                                   link_settings.diagnostics);
    update_link_control_state(state);
    refresh_voxel_profile_controls(state);
    state.shortcuts_heading = control(
        state, L"STATIC", L"Keyboard shortcuts", 0,
        32, 200, 360, 30, 0);
    SendMessageW(state.shortcuts_heading, WM_SETFONT,
                 reinterpret_cast<WPARAM>(state.title_font), TRUE);
    const auto shortcut_reference = shortcuts_text(state);
    state.shortcuts_text = control(
        state, L"EDIT", shortcut_reference.c_str(),
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        32, 240, 916, 565, 0);
    state.settings_content_controls = {
        state.palette_label,
        state.palette,
        state.video_label,
        state.video,
        state.hardware_model_label,
        state.hardware_model,
        state.audio_enabled,
        state.controls_label,
        state.controls_instruction,
        state.actions_label,
        state.reset_controls,
        state.voxel_heading,
        state.voxel_fingerprint_label,
        state.voxel_preview,
        state.voxel_save,
        state.voxel_reset,
        state.plugin_heading,
        state.plugin_status,
        state.plugin_discovery,
        state.plugin_require_allowlist,
        state.plugin_require_capability_allowlist,
        state.link_heading,
        state.link_transport_label,
        state.link_transport,
        state.link_remote_host_label,
        state.link_remote_host,
        state.link_remote_bind_label,
        state.link_remote_bind,
        state.link_remote_port_label,
        state.link_remote_port,
        state.link_lan_discovery,
        state.link_bluetooth_address_label,
        state.link_bluetooth_address,
        state.link_bluetooth_choose,
        state.link_bluetooth_uuid_label,
        state.link_bluetooth_uuid,
        state.link_diagnostics};
    for (const auto heading : state.primary_headings) {
        state.settings_content_controls.push_back(heading);
    }
    for (const auto heading : state.secondary_headings) {
        state.settings_content_controls.push_back(heading);
    }
    for (const auto label : state.binding_labels) {
        state.settings_content_controls.push_back(label);
    }
    for (const auto& buttons : state.binding_buttons) {
        for (const auto button : buttons) {
            state.settings_content_controls.push_back(button);
        }
    }
    for (const auto label : state.action_labels) {
        state.settings_content_controls.push_back(label);
    }
    for (const auto button : state.action_buttons) {
        state.settings_content_controls.push_back(button);
    }
    for (const auto label : state.voxel_labels) {
        state.settings_content_controls.push_back(label);
    }
    for (const auto edit : state.voxel_edits) {
        state.settings_content_controls.push_back(edit);
    }
    // The owner-drawn controller illustration is created before the settings
    // widgets and covers their rectangle. Keep the model selector above it so
    // it remains clickable on the settings page.
    SetWindowPos(state.hardware_model_label, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(state.hardware_model, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    refresh_binding_buttons(state);
    layout_dashboard(state);
    show_page(state, State::Page::library);
    if (ListView_GetItemCount(state.list) > 0) {
        ListView_SetItemState(state.list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        // Make the visible search field the initial keyboard target so typing
        // immediately filters the library instead of moving the list cursor.
        SetFocus(state.search);
    } else {
        SetFocus(state.open);
    }
    refresh_library_actions(state);

    // Creating and populating child controls can emit change notifications.
    // Those are initialization traffic, not user edits, so an untouched
    // dashboard must close without an unsaved-settings prompt.
    state.settings_dirty = false;
    SetWindowTextW(
        state.settings_status,
        L"Changes are staged until you apply or discard them.");

    if (owner != nullptr) EnableWindow(owner, FALSE);
    ShowWindow(state.window, SW_SHOW);
    UpdateWindow(state.window);
    start_artwork_resolution(state);
    if (state.poll_update) {
        // GetMessageW blocks when the dashboard is idle. A timer keeps the
        // background updater observable without spinning the UI thread.
        SetTimer(state.window, update_poll_timer, 50, nullptr);
    }
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        // WM_MOUSEWHEEL is often dispatched to whichever child control is
        // under the pointer. Handle it here as well as in the parent window
        // procedure so settings scrolls consistently over edits and buttons.
        if (message.message == WM_MOUSEWHEEL &&
            state.page == State::Page::settings) {
            scroll_settings(state, GET_WHEEL_DELTA_WPARAM(message.wParam));
            continue;
        }
        if ((message.message == WM_KEYDOWN ||
             message.message == WM_SYSKEYDOWN) &&
            message.wParam == VK_F1 && !state.capturing_binding) {
            show_page(state, State::Page::shortcuts);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    state.closing = true;
    if (state.artwork_worker.joinable()) state.artwork_worker.join();
    if (owner != nullptr) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return state.result;
}

} // namespace gbb_desktop
#endif
