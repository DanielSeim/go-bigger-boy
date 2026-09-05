#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "windows_dashboard.hpp"
#include "resource.h"
#include "update_checker.hpp"

#ifdef _WIN32

#include <SDL3/SDL.h>

#include <commctrl.h>
#include <commdlg.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <ctime>
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
constexpr int id_palette = 107;
constexpr int id_remove = 108;
constexpr int id_video = 109;
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
constexpr int id_binding_first = 200;
constexpr int id_action_first = 220;
constexpr UINT artwork_ready = WM_APP + 1;
constexpr int dashboard_width = 980;
constexpr int dashboard_height = 1120;

struct MetadataRecord {
    std::string name;
    std::string language;
};

struct ArtworkUpdate {
    std::size_t index{};
    std::string title;
    std::string language;
    std::filesystem::path cover;
};

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

struct State {
    const gameboy::RomLibrary* library{};
    DashboardResult result;
    bool can_resume{};
    bool voxel_available{};
    bool done{};
    HWND window{};
    HWND list{};
    HWND library_empty{};
    HWND play{};
    HWND open{};
    HWND resume{};
    HWND remove{};
    HWND palette{};
    HWND settings_heading{};
    HWND palette_label{};
    HWND video_label{};
    HWND video{};
    HWND controls_label{};
    HWND controls_instruction{};
    HWND actions_label{};
    HWND gameboy_background{};
    std::array<HWND, 8> binding_labels{};
    std::array<HWND, 2> primary_headings{};
    std::array<HWND, 2> secondary_headings{};
    std::array<std::array<HWND, 2>, 8> binding_buttons{};
    std::array<HWND, 4> action_labels{};
    std::array<HWND, 4> action_buttons{};
    HWND reset_controls{};
    HWND voxel_heading{};
    HWND voxel_fingerprint_label{};
    HWND voxel_preview{};
    std::array<HWND, 8> voxel_labels{};
    std::array<HWND, 8> voxel_edits{};
    HWND voxel_save{};
    HWND voxel_reset{};
    HWND plugin_heading{};
    HWND plugin_status{};
    HWND plugin_discovery{};
    HWND plugin_require_allowlist{};
    HWND plugin_require_capability_allowlist{};
    HWND library_tab{};
    HWND settings_tab{};
    HWND shortcuts_tab{};
    HWND artwork_status{};
    HWND shortcuts_heading{};
    HWND shortcuts_text{};
    HWND logo{};
    HBITMAP logo_bitmap{};
    HIMAGELIST covers{};
    HBRUSH background_brush{};
    HFONT ui_font{};
    enum class Page { library, settings, shortcuts } page{Page::library};
    std::filesystem::path preference_directory;
    std::filesystem::path voxel_profile_path;
    gbb::PluginDiscoveryOptions plugin_options;
    std::wstring plugin_status_text;
    std::uint64_t voxel_fingerprint{};
    gbb::VoxelProfile voxel_profile{};
    std::thread artwork_worker;
    std::atomic_bool closing{};
    DownloadProgress artwork_download;
    std::atomic_size_t artwork_completed{};
    std::size_t artwork_total{};
    int settings_scroll{};
    HFONT title_font{};
    struct CapturingBinding {
        bool action{};
        std::size_t index{};
        std::size_t slot{};
        friend constexpr bool operator==(const CapturingBinding& left,
                                        const CapturingBinding& right) {
            return left.action == right.action && left.index == right.index &&
                   left.slot == right.slot;
        }
    };
    std::optional<CapturingBinding> capturing_binding;
};

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

std::wstring binding_name(const std::int64_t value) {
    if (value == SDLK_UNKNOWN) return L"None";
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
        L"F11: Toggle fullscreen\r\n"
        L"F12: Open or close the debugger\r\n"
        L"Escape: Close with confirmation\r\n"
        L"\r\nDEBUGGER\r\n"
        L"F5: Run or pause\r\n"
        L"F6: Start or stop input recording\r\n"
        L"F7: Replay the latest input movie\r\n"
        L"F8: Open the TAS frame editor\r\n"
        L"F9: Open the live sprite editor\r\n"
        L"F10: Step one CPU instruction\r\n"
        L"F11: Step one frame\r\n"
        L"F12 or Escape: Close the debugger\r\n"
        L"Click a CPU register: Edit its hexadecimal value\r\n"
        L"\r\nTAS FRAME EDITOR\r\n"
        L"Up / Down: Select a frame\r\n"
        L"Insert: Insert an empty frame\r\n"
        L"Delete: Delete the selected frame\r\n"
        L"End: Append an empty frame\r\n"
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

constexpr std::array<const wchar_t*, 8> voxel_profile_names{{
    L"Depth scale", L"Camera pitch", L"Camera yaw", L"Zoom",
    L"Perspective", L"Sprite depth", L"Lighting",
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
    const std::array<float, 8> values{{
        state.voxel_profile.depth_scale, state.voxel_profile.camera_pitch,
        state.voxel_profile.camera_yaw, state.voxel_profile.zoom,
        state.voxel_profile.perspective, state.voxel_profile.sprite_depth,
        state.voxel_profile.lighting,
        state.voxel_profile.framebuffer_facade ? 1.0F : 0.0F}};
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index == 7) {
            SendMessageW(state.voxel_edits[index], BM_SETCHECK,
                         values[index] >= 0.5F ? BST_CHECKED : BST_UNCHECKED, 0);
        } else {
            SetWindowTextW(state.voxel_edits[index],
                           voxel_float_text(values[index]).c_str());
        }
    }
    invalidate_voxel_preview(state);
}

bool read_voxel_profile_controls(State& state) {
    std::array<float, 8> values{{
        state.voxel_profile.depth_scale, state.voxel_profile.camera_pitch,
        state.voxel_profile.camera_yaw, state.voxel_profile.zoom,
        state.voxel_profile.perspective, state.voxel_profile.sprite_depth,
        state.voxel_profile.lighting,
        state.voxel_profile.framebuffer_facade ? 1.0F : 0.0F}};
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index == 7) {
            values[index] = SendMessageW(state.voxel_edits[index], BM_GETCHECK,
                                         0, 0) == BST_CHECKED ? 1.0F : 0.0F;
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
    state.voxel_profile.framebuffer_facade = values[7] >= 0.5F;
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
    SetWindowTextW(state.controls_instruction,
                   L"Click a binding, then press a key. Delete clears a binding.");
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
void layout_dashboard(State& state);

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
    ShowWindow(state.artwork_status, library ? SW_SHOW : SW_HIDE);
    if (state.library_empty != nullptr) {
        const auto empty = library && ListView_GetItemCount(state.list) == 0;
        ShowWindow(state.library_empty, empty ? SW_SHOW : SW_HIDE);
    }
    ShowWindow(state.play, library ? SW_SHOW : SW_HIDE);
    ShowWindow(state.open, library ? SW_SHOW : SW_HIDE);
    ShowWindow(state.remove, library ? SW_SHOW : SW_HIDE);
    ShowWindow(state.resume, library && state.can_resume ? SW_SHOW : SW_HIDE);
    ShowWindow(state.settings_heading, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.palette_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.palette, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.video_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.video, settings ? SW_SHOW : SW_HIDE);
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
    ShowWindow(state.shortcuts_heading, shortcuts ? SW_SHOW : SW_HIDE);
    ShowWindow(state.shortcuts_text, shortcuts ? SW_SHOW : SW_HIDE);
    ShowScrollBar(state.window, SB_VERT, settings ? TRUE : FALSE);
    layout_dashboard(state);
    if (library) refresh_library_actions(state);
    if (settings) {
        // The owner-drawn Game Boy artwork overlaps these controls. Keep the
        // bindings above it and repaint them immediately when opening Settings.
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

void place_child(HWND child, int x, int y, int width, int height,
                 const int scroll) {
    if (child == nullptr) return;
    SetWindowPos(child, nullptr, x, y - scroll, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void layout_dashboard(State& state) {
    if (state.window == nullptr) return;
    RECT client{};
    GetClientRect(state.window, &client);
    const auto width = std::max(720L, client.right - client.left);
    const auto height = std::max(520L, client.bottom - client.top);
    const auto content_width = std::max(400L, width - 64L);
    const auto list_height = std::max(220L, height - 300L);
    place_child(state.list, 32, 200, static_cast<int>(content_width),
                static_cast<int>(list_height), 0);
    place_child(state.library_empty, 32, 300, static_cast<int>(content_width),
                100, 0);
    place_child(state.artwork_status, 32, 180, static_cast<int>(content_width),
                20, 0);
    const auto actions_y = 200L + list_height + 25L;
    place_child(state.open, 32, static_cast<int>(actions_y), 150, 44, 0);
    place_child(state.play, 202, static_cast<int>(actions_y), 160, 44, 0);
    place_child(state.remove, 382, static_cast<int>(actions_y), 170, 44, 0);
    place_child(state.resume, 572, static_cast<int>(actions_y), 150, 44, 0);

    const auto content_bottom = 1245L;
    const auto max_scroll = std::max(0L, content_bottom - height + 24L);
    state.settings_scroll = std::clamp(state.settings_scroll, 0,
                                       static_cast<int>(max_scroll));
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS,
                      0, static_cast<int>(content_bottom),
                      static_cast<UINT>(height), state.settings_scroll, 0};
    SetScrollInfo(state.window, SB_VERT, &scroll, TRUE);
    const auto offset = state.settings_scroll;
    place_child(state.settings_heading, 32, 200, 360, 30, offset);
    place_child(state.palette_label, 32, 245, 110, 26, offset);
    place_child(state.palette, 154, 240, 290, 26, offset);
    place_child(state.video_label, 32, 275, 110, 26, offset);
    place_child(state.video, 154, 270, 290, 26, offset);
    place_child(state.controls_label, 510, 200, 240, 30, offset);
    place_child(state.controls_instruction, 510, 238, 420, 26, offset);
    place_child(state.gameboy_background, 32, 310, 916, 268, offset);
    for (int column = 0; column < 2; ++column) {
        const auto base_x = column == 0 ? 72 : 520;
        place_child(state.primary_headings[static_cast<std::size_t>(column)],
                    base_x + 78, 316, 120, 24, offset);
        place_child(state.secondary_headings[static_cast<std::size_t>(column)],
                    base_x + 214, 316, 120, 24, offset);
    }
    constexpr std::array<std::size_t, 8> order{{2, 1, 0, 3, 4, 5, 6, 7}};
    for (std::size_t position = 0; position < order.size(); ++position) {
        const auto index = order[position];
        const auto column = position < 4 ? 0 : 1;
        const auto row = static_cast<int>(position % 4);
        const auto base_x = column == 0 ? 72 : 520;
        const auto y = 350 + row * 48;
        place_child(state.binding_labels[index], base_x, y + 8, 64, 26, offset);
        for (std::size_t slot = 0; slot < 2; ++slot) {
            place_child(state.binding_buttons[index][slot],
                        base_x + 78 + static_cast<int>(slot) * 136, y, 120, 38,
                        offset);
        }
    }
    place_child(state.actions_label, 32, 610, 260, 28, offset);
    for (std::size_t index = 0; index < state.action_labels.size(); ++index) {
        const auto column = index % 2;
        const auto row = index / 2;
        const auto base_x = column == 0 ? 72 : 520;
        const auto y = 650 + static_cast<int>(row) * 50;
        place_child(state.action_labels[index], base_x, y + 8, 130, 26, offset);
        place_child(state.action_buttons[index], base_x + 145, y, 150, 38,
                    offset);
    }
    place_child(state.reset_controls, 32, 775, 230, 40, offset);
    place_child(state.voxel_heading, 32, 825, 320, 28, offset);
    place_child(state.voxel_fingerprint_label, 360, 827, 580, 24, offset);
    place_child(state.voxel_preview, 300, 850, 180, 150, offset);
    constexpr std::array<int, 8> profile_x{{32, 32, 32, 32, 510, 510, 510, 510}};
    constexpr std::array<int, 8> profile_y{{850, 890, 930, 970, 850, 890, 930, 970}};
    for (std::size_t index = 0; index < state.voxel_labels.size(); ++index) {
        place_child(state.voxel_labels[index], profile_x[index], profile_y[index],
                    120, 24, offset);
        place_child(state.voxel_edits[index], profile_x[index] + 130,
                    profile_y[index] - 2, 130, 28, offset);
    }
    place_child(state.voxel_save, 680, 1020, 120, 38, offset);
    place_child(state.voxel_reset, 810, 1020, 120, 38, offset);
    place_child(state.plugin_heading, 32, 1080, 320, 28, offset);
    place_child(state.plugin_status, 32, 1115, 916, 72, offset);
    place_child(state.plugin_discovery, 32, 1195, 260, 28, offset);
    place_child(state.plugin_require_allowlist, 320, 1195, 320, 28, offset);
    place_child(state.plugin_require_capability_allowlist, 660, 1195, 290, 28,
                offset);
}

void finish(State& state, const DashboardResultAction action,
            const std::string& path = {}) {
    save_window_position(state);
    state.result.action = action;
    state.result.rom_path = path;
    state.closing = true;
    state.artwork_download.cancel_requested.store(true,
                                                  std::memory_order_relaxed);
    state.done = true;
    DestroyWindow(state.window);
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
            SetWindowTextW(state->artwork_status, L"Artwork: ready");
            KillTimer(window, 1);
        } else {
            const auto text = L"Artwork: loading " +
                              std::to_wstring(completed) + L" of " +
                              std::to_wstring(state->artwork_total) +
                              L"... (close to cancel)";
            SetWindowTextW(state->artwork_status, text.c_str());
        }
        return 0;
    }
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize.x = 720;
        limits->ptMinTrackSize.y = 520;
        return 0;
    }
    if (message == WM_VSCROLL) {
        RECT client{};
        GetClientRect(window, &client);
        const auto max_scroll = std::max(
            0L, 1058L - static_cast<long>(client.bottom - client.top) + 24L);
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
        state->settings_scroll = std::clamp(next, 0, static_cast<int>(max_scroll));
        layout_dashboard(*state);
        return 0;
    }
    if (message == WM_MOUSEWHEEL && state->page == State::Page::settings) {
        const auto delta = GET_WHEEL_DELTA_WPARAM(wparam);
        state->settings_scroll = std::max(
            0, state->settings_scroll - (delta > 0 ? 64 : -64));
        layout_dashboard(*state);
        return 0;
    }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORLISTBOX ||
        message == WM_CTLCOLOREDIT) {
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
                L"Click a binding, then press a key. Delete clears a binding.");
            refresh_binding_buttons(*state);
            return 0;
        }
        const auto capture = *state->capturing_binding;
        if (wparam == VK_DELETE &&
            (capture.action || capture.slot == 1)) {
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
        if (confirm_exit(window)) finish(*state, DashboardResultAction::quit);
        return 0;
    }

    if (message == WM_COMMAND) {
        const auto command = LOWORD(wparam);
        if (command >= id_voxel_first_edit &&
            command < id_voxel_first_edit + 8 && state->voxel_available) {
            const auto index = static_cast<std::size_t>(
                command - id_voxel_first_edit);
            const auto notification = HIWORD(wparam);
            const auto editing = index == 7 ? notification == BN_CLICKED
                                            : notification == EN_CHANGE;
            if (editing && read_voxel_profile_controls(*state)) {
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
                }
            }
            return 0;
        case id_plugin_discovery:
            if (HIWORD(wparam) == BN_CLICKED) {
                state->result.plugin_discovery =
                    SendMessageW(state->plugin_discovery, BM_GETCHECK, 0, 0) ==
                    BST_CHECKED;
                state->result.plugin_settings_changed = true;
                SetWindowTextW(
                    state->plugin_status,
                    L"Plugin setting changed. Restart the emulator to reload plugins.");
            }
            return 0;
        case id_plugin_require_allowlist:
            if (HIWORD(wparam) == BN_CLICKED) {
                state->result.plugin_require_allowlist =
                    SendMessageW(state->plugin_require_allowlist, BM_GETCHECK,
                                 0, 0) == BST_CHECKED;
                state->result.plugin_settings_changed = true;
                SetWindowTextW(
                    state->plugin_status,
                    L"Plugin trust policy changed. Restart the emulator to reload plugins.");
            }
            return 0;
        case id_plugin_require_capability_allowlist:
            if (HIWORD(wparam) == BN_CLICKED) {
                state->result.plugin_require_capability_allowlist =
                    SendMessageW(state->plugin_require_capability_allowlist,
                                 BM_GETCHECK, 0, 0) == BST_CHECKED;
                state->result.plugin_settings_changed = true;
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
            SetWindowTextW(state->voxel_fingerprint_label,
                           L"Voxel profile saved. Return to the game to preview it.");
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
            return 0;
        default: break;
        }
    } else if (message == WM_DRAWITEM) {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (wparam == id_voxel_preview) {
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
        if (confirm_exit(window)) {
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

std::string trimmed(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value.erase(0, first);
    const auto last = value.find_last_not_of(" \t\r\n");
    value.resize(last + 1);
    return value;
}

std::string quoted_value(const std::string& line) {
    const auto first = line.find('"');
    const auto last = line.rfind('"');
    return first != std::string::npos && last > first
               ? line.substr(first + 1, last - first - 1)
               : std::string{};
}

std::string lowercase(std::string value) {
    for (auto& character : value) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string metadata_language(const std::string& name,
                              const std::string& region) {
    const auto lower_name = lowercase(name);
    constexpr std::array<std::pair<std::string_view, std::string_view>, 14>
        tags{{{"(de", "German"}, {",de", "German"},
              {"(en", "English"}, {",en", "English"},
              {"(fr", "French"}, {",fr", "French"},
              {"(es", "Spanish"}, {",es", "Spanish"},
              {"(it", "Italian"}, {",it", "Italian"},
              {"(nl", "Dutch"}, {",nl", "Dutch"},
              {"(ja", "Japanese"}, {",ja", "Japanese"}}};
    for (const auto& [tag, language] : tags) {
        if (lower_name.find(tag) != std::string::npos) {
            return std::string{language};
        }
    }
    const auto lower_region = lowercase(region);
    constexpr std::array<std::pair<std::string_view, std::string_view>, 10>
        regions{{{"germany", "German"}, {"france", "French"},
                 {"spain", "Spanish"}, {"italy", "Italian"},
                 {"netherlands", "Dutch"}, {"japan", "Japanese"},
                 {"usa", "English"}, {"europe", "English"},
                 {"australia", "English"}, {"canada", "English"}}};
    for (const auto& [country, language] : regions) {
        if (lower_region.find(country) != std::string::npos) {
            return std::string{language};
        }
    }
    return "International";
}

std::unordered_map<std::uint32_t, MetadataRecord> parse_database(
    const std::filesystem::path& path) {
    std::unordered_map<std::uint32_t, MetadataRecord> records;
    std::ifstream input(path);
    std::string line;
    std::string name;
    std::string region;
    while (std::getline(input, line)) {
        line = trimmed(std::move(line));
        if (line == "game (") {
            name.clear();
            region.clear();
        } else if (line.rfind("name \"", 0) == 0 && name.empty()) {
            name = quoted_value(line);
        } else if (line.rfind("region \"", 0) == 0) {
            region = quoted_value(line);
        } else if (line.rfind("rom (", 0) == 0 && !name.empty()) {
            const auto marker = line.find(" crc ");
            if (marker == std::string::npos || marker + 13 > line.size()) {
                continue;
            }
            std::uint32_t crc{};
            std::istringstream value(line.substr(marker + 5, 8));
            if (value >> std::hex >> crc) {
                records.emplace(crc,
                    MetadataRecord{name, metadata_language(name, region)});
            }
        }
    }
    return records;
}

std::string url_component(const std::string& value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            encoded << character;
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<unsigned>(byte);
        }
    }
    return encoded.str();
}

std::string thumbnail_name(std::string name) {
    constexpr std::string_view replaced = "&*/:`<>?\\|";
    for (auto& character : name) {
        if (replaced.find(character) != std::string_view::npos) character = '_';
    }
    return name;
}

std::string display_title(const std::string& canonical_name) {
    const auto tags = canonical_name.find(" (");
    return tags == std::string::npos ? canonical_name
                                     : canonical_name.substr(0, tags);
}

std::unordered_map<std::uint32_t, MetadataRecord> load_database(
    const std::filesystem::path& directory, const std::string& system,
    DownloadProgress* progress) {
    auto filename = system;
    for (auto& character : filename) {
        if (!std::isalnum(static_cast<unsigned char>(character))) character = '-';
    }
    const auto path = directory / "metadata" / (filename + ".dat");
    if (!std::filesystem::is_regular_file(path)) {
        std::string error;
        const auto url =
            "https://raw.githubusercontent.com/libretro/libretro-database/"
            "master/metadat/no-intro/" + url_component(system + ".dat");
        static_cast<void>(download_public_file(url, path, 3 * 1024 * 1024,
                                               error, progress));
    }
    return parse_database(path);
}

void resolve_artwork(State& state) {
    std::unordered_map<std::string,
        std::unordered_map<std::uint32_t, MetadataRecord>> databases;
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
                system, load_database(state.preference_directory, system,
                                      &state.artwork_download)).first;
        }
        auto title = metadata.title;
        auto language = metadata.language;
        auto canonical = metadata.cover_name;
        if (const auto record = found_database->second.find(metadata.crc32);
            record != found_database->second.end()) {
            canonical = record->second.name;
            title = display_title(canonical);
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
                url_component(system) + "/Named_Boxarts/" +
                url_component(thumbnail_name(canonical)) + ".png";
            static_cast<void>(download_public_file(
                url, cover, 5 * 1024 * 1024, error, &state.artwork_download));
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

HWND control(State& state, const wchar_t* type, const wchar_t* text,
             DWORD style, int x, int y, int width, int height, int id) {
    if (std::wstring_view(type) == L"BUTTON" &&
        (style & BS_AUTOCHECKBOX) == 0) {
        style |= BS_OWNERDRAW;
    }
    const auto control_type = std::wstring_view(type);
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
    const auto fill = disabled
                          ? RGB(28, 34, 45)
                          : active_tab || pressed ? RGB(0, 145, 190)
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

} // namespace

DashboardResult show_windows_dashboard(
    HWND owner, const gameboy::RomLibrary& library, const bool can_resume,
    const std::uint64_t current_fingerprint,
    const gbb::CoreCapability capabilities,
    const std::size_t palette, const gameboy::VideoMode video_mode,
    const KeyboardBindings& keyboard_bindings,
    const ActionBindings& action_bindings,
    const gbb::PluginDiscoveryOptions& plugin_options,
    const gbb::PluginCatalog& plugin_catalog,
    const std::filesystem::path& preference_directory) {
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
    state.result.palette = palette;
    state.result.video_mode = video_mode;
    state.result.keyboard_bindings = keyboard_bindings;
    state.result.action_bindings = action_bindings;
    state.result.plugin_discovery = plugin_options.enabled;
    state.result.plugin_require_allowlist = plugin_options.require_allowlist;
    state.result.plugin_require_capability_allowlist =
        plugin_options.require_capability_allowlist;
    state.plugin_options = plugin_options;
    state.plugin_status_text = plugin_status_text(plugin_options, plugin_catalog);
    state.preference_directory = preference_directory;
    const auto saved_position = load_window_position(preference_directory);
    const auto window_title = std::wstring{L"Go Bigger Boy - Game Library v"} +
                              widen(GBB_VERSION);
    state.window = CreateWindowExW(
        WS_EX_APPWINDOW, class_name, window_title.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_THICKFRAME | WS_VSCROLL,
        saved_position ? saved_position->x : CW_USEDEFAULT,
        saved_position ? saved_position->y : CW_USEDEFAULT,
        dashboard_width, dashboard_height, owner, nullptr, instance, &state);
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
    state.artwork_status = control(
        state, L"STATIC", L"Artwork: loading...", WS_VISIBLE,
        32, 180, 916, 20, 0);
    state.list = control(state, WC_LISTVIEWW, L"",
        WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        32, 200, 916, 350, id_list);
    state.library_empty = control(
        state, L"STATIC",
        L"No games yet.\n\nChoose Open ROM... to add a game to your library.",
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
    constexpr std::array<std::pair<const wchar_t*, int>, 5> columns{{
        {L"Cover", 62}, {L"Game", 290}, {L"Platform", 150},
        {L"Language", 150}, {L"Last played", 220}}};
    for (std::size_t index = 0; index < columns.size(); ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(columns[index].first);
        column.cx = columns[index].second;
        SendMessageW(state.list, LVM_INSERTCOLUMNW,
                     static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&column));
    }
    for (std::size_t index = 0; index < library.entries().size(); ++index) {
        const auto& entry = library.entries()[index];
        auto title = widen(entry.metadata.title);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(L"");
        item.iImage = 0;
        item.lParam = static_cast<LPARAM>(index);
        SendMessageW(state.list, LVM_INSERTITEMW, 0,
                     reinterpret_cast<LPARAM>(&item));
        LVITEMW subitem{};
        subitem.iSubItem = 1;
        subitem.pszText = title.data();
        SendMessageW(state.list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&subitem));
        auto platform_name = widen(gameboy::platform_name(entry.metadata.platform));
        subitem.iSubItem = 2;
        subitem.pszText = platform_name.data();
        SendMessageW(state.list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&subitem));
        auto language = widen(entry.metadata.language);
        subitem.iSubItem = 3;
        subitem.pszText = language.data();
        SendMessageW(state.list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&subitem));
        auto last_played = formatted_last_played(entry.last_played);
        subitem.iSubItem = 4;
        subitem.pszText = last_played.data();
        SendMessageW(state.list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&subitem));
    }
    state.open = control(state, L"BUTTON", L"Open ROM...",
        WS_VISIBLE | BS_PUSHBUTTON, 32, 575, 150, 44, id_open);
    state.play = control(state, L"BUTTON", L"Play selected",
        WS_VISIBLE | BS_DEFPUSHBUTTON, 202, 575, 160, 44, id_play);
    state.remove = control(state, L"BUTTON", L"Remove from list",
        WS_VISIBLE | BS_PUSHBUTTON, 382, 575, 170, 44, id_remove);
    state.resume = control(state, L"BUTTON", L"Resume game",
        (can_resume ? WS_VISIBLE : 0) | BS_PUSHBUTTON,
        572, 575, 150, 44, id_resume);
    state.settings_heading = control(state, L"STATIC", L"Display and controls",
        0, 32, 200, 360, 30, 0);
    SendMessageW(state.settings_heading, WM_SETFONT,
                 reinterpret_cast<WPARAM>(state.title_font), TRUE);
    state.palette_label = control(state, L"STATIC", L"Color palette",
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
    state.video_label = control(state, L"STATIC", L"Video pipeline",
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
    state.controls_label = control(state, L"STATIC", L"Keyboard controls",
        0, 510, 200, 240, 30, 0);
    SendMessageW(state.controls_label, WM_SETFONT,
                 reinterpret_cast<WPARAM>(state.title_font), TRUE);
    state.controls_instruction = control(
        state, L"STATIC",
        L"Click a binding, then press a key. Delete clears a binding.",
        0, 510, 238, 420, 26, 0);
    state.gameboy_background = control(
        state, L"STATIC", L"", SS_OWNERDRAW | WS_CLIPSIBLINGS,
        32, 310, 916, 268,
        id_gameboy_background);

    for (int column = 0; column < 2; ++column) {
        const auto base_x = column == 0 ? 72 : 520;
        state.primary_headings[static_cast<std::size_t>(column)] = control(
            state, L"STATIC", L"Primary", 0,
            base_x + 78, 316, 120, 24, 0);
        state.secondary_headings[static_cast<std::size_t>(column)] = control(
            state, L"STATIC", L"Secondary", 0,
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
    const auto profile_columns = std::array<int, 8>{{32, 32, 32, 32,
                                                       510, 510, 510, 510}};
    const auto profile_rows = std::array<int, 8>{{850, 890, 930, 970,
                                                   850, 890, 930, 970}};
    for (std::size_t index = 0; index < voxel_profile_names.size(); ++index) {
        const auto x = profile_columns[index];
        const auto y = profile_rows[index];
        state.voxel_labels[index] = control(
            state, L"STATIC", voxel_profile_names[index], 0,
            x, y, 120, 24, 0);
        state.voxel_edits[index] = index == 7
            ? control(state, L"BUTTON", L"Enabled", BS_AUTOCHECKBOX,
                      x + 130, y - 2, 130, 28,
                      id_voxel_first_edit + static_cast<int>(index))
            : control(state, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                      x + 130, y - 2, 130, 28,
                      id_voxel_first_edit + static_cast<int>(index));
    }
    state.voxel_save = control(state, L"BUTTON", L"Save profile",
                               BS_PUSHBUTTON, 680, 1020, 120, 38,
                               id_voxel_save);
    state.voxel_reset = control(state, L"BUTTON", L"Reset profile",
                                BS_PUSHBUTTON, 810, 1020, 120, 38,
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
    SendMessageW(state.plugin_discovery, BM_SETCHECK,
                 plugin_options.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.plugin_require_allowlist, BM_SETCHECK,
                 plugin_options.require_allowlist ? BST_CHECKED
                                                   : BST_UNCHECKED,
                 0);
    SendMessageW(state.plugin_require_capability_allowlist, BM_SETCHECK,
                 plugin_options.require_capability_allowlist ? BST_CHECKED
                                                              : BST_UNCHECKED,
                 0);
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
    refresh_binding_buttons(state);
    layout_dashboard(state);
    show_page(state, State::Page::library);
    if (ListView_GetItemCount(state.list) > 0) {
        ListView_SetItemState(state.list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        SetFocus(state.list);
    } else {
        SetFocus(state.open);
    }
    refresh_library_actions(state);

    if (owner != nullptr) EnableWindow(owner, FALSE);
    ShowWindow(state.window, SW_SHOW);
    UpdateWindow(state.window);
    state.artwork_total = library.entries().size();
    state.artwork_completed = 0;
    if (state.artwork_total == 0) {
        SetWindowTextW(state.artwork_status, L"Artwork: ready");
    } else {
        SetTimer(state.window, 1, 100, nullptr);
    }
    state.artwork_worker = std::thread([&state] { resolve_artwork(state); });
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
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
