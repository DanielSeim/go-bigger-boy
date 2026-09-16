#include "windows_dashboard_smoke.hpp"

#ifdef _WIN32

#include "windows_dashboard.hpp"

#include "gbb/core_registry.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace gbb_desktop {
namespace {

using Clock = std::chrono::steady_clock;

struct DashboardInvocation {
    gameboy::RomLibrary library;
    DashboardResult result;
    std::mutex mutex;
    bool completed{};
};

bool wait_for(const std::function<bool()>& predicate,
              const std::chrono::milliseconds timeout =
                  std::chrono::milliseconds{5000}) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (predicate()) return true;
        Sleep(10);
    }
    return predicate();
}

BOOL CALLBACK find_dashboard_window(HWND window, LPARAM data) {
    wchar_t class_name[64]{};
    GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
    if (std::wstring_view{class_name} == L"GoBiggerBoyDashboard") {
        *reinterpret_cast<HWND*>(data) = window;
        return FALSE;
    }
    return TRUE;
}

HWND dashboard_window() {
    HWND result = nullptr;
    EnumWindows(find_dashboard_window, reinterpret_cast<LPARAM>(&result));
    return result;
}

struct TextSearch {
    std::wstring text;
    std::wstring class_name;
    HWND result{};
};

BOOL CALLBACK find_child_by_text(HWND child, LPARAM data) {
    auto& search = *reinterpret_cast<TextSearch*>(data);
    wchar_t child_class[64]{};
    GetClassNameW(child, child_class,
                  static_cast<int>(std::size(child_class)));
    if (!search.class_name.empty() &&
        _wcsicmp(child_class, search.class_name.c_str()) != 0) {
        return TRUE;
    }
    wchar_t text[256]{};
    GetWindowTextW(child, text, static_cast<int>(std::size(text)));
    if (std::wstring_view{text} == search.text) {
        search.result = child;
        return FALSE;
    }
    return TRUE;
}

HWND child_by_text(HWND window, const wchar_t* text,
                   const wchar_t* class_name = nullptr) {
    TextSearch search{std::wstring{text},
                      class_name == nullptr ? std::wstring{}
                                             : std::wstring{class_name},
                      nullptr};
    EnumChildWindows(window, find_child_by_text,
                     reinterpret_cast<LPARAM>(&search));
    return search.result;
}

bool click_child(HWND window, const wchar_t* text,
                 const wchar_t* class_name = L"BUTTON") {
    auto child = HWND{};
    if (!wait_for([&] {
            child = child_by_text(window, text, class_name);
            return child != nullptr;
        })) {
        return false;
    }
    SendMessageW(child, BM_CLICK, 0, 0);
    return true;
}

bool dashboard_completed(DashboardInvocation& invocation) {
    std::lock_guard lock{invocation.mutex};
    return invocation.completed;
}

bool confirm_message_box(const wchar_t* title, const UINT command) {
    HWND dialog = nullptr;
    if (!wait_for([&] {
            dialog = FindWindowW(L"#32770", title);
            return dialog != nullptr;
        })) {
        return false;
    }
    PostMessageW(dialog, WM_COMMAND, command, 0);
    return true;
}

void close_dashboard(HWND window) {
    if (window == nullptr) return;
    PostMessageW(window, windows_dashboard_smoke_close, 0, 0);
}

struct VisibleControl {
    HWND window{};
    std::wstring class_name;
    RECT rect{};
};

struct ControlCollection {
    HWND dashboard{};
    std::vector<VisibleControl> controls;
};

bool class_name_is(const wchar_t* actual, const wchar_t* expected) {
    return _wcsicmp(actual, expected) == 0;
}

BOOL CALLBACK collect_visible_controls(HWND child, LPARAM data) {
    auto& collection = *reinterpret_cast<ControlCollection*>(data);
    if (!IsWindowVisible(child)) return TRUE;
    wchar_t class_name[64]{};
    GetClassNameW(child, class_name,
                  static_cast<int>(std::size(class_name)));
    // The dashboard explicitly gives every interactive control a tab stop.
    // Use that stable behavior rather than depending on runner-specific
    // Win32 class names or dialog-ID propagation through child hierarchies.
    const auto style = GetWindowLongPtrW(child, GWL_STYLE);
    if ((style & WS_TABSTOP) == 0) {
        return TRUE;
    }
    RECT screen_rect{};
    if (!GetWindowRect(child, &screen_rect)) return TRUE;
    POINT origin{screen_rect.left, screen_rect.top};
    MapWindowPoints(nullptr, collection.dashboard, &origin, 1);
    collection.controls.push_back(
        {child, std::wstring{class_name},
         {origin.x, origin.y, origin.x + screen_rect.right - screen_rect.left,
          origin.y + screen_rect.bottom - screen_rect.top}});
    return TRUE;
}

bool rectangles_overlap(const RECT& first, const RECT& second) {
    RECT intersection{};
    return IntersectRect(&intersection, &first, &second) != FALSE;
}

bool check_native_controls_and_layout(HWND dashboard) {
    ControlCollection collection{dashboard};
    EnumChildWindows(dashboard, collect_visible_controls,
                     reinterpret_cast<LPARAM>(&collection));
    RECT client{};
    if (!GetClientRect(dashboard, &client)) return false;

    bool has_owner_drawn_checkbox = false;
    bool has_owner_drawn_combo = false;
    for (const auto& control : collection.controls) {
        if (control.rect.left < client.left || control.rect.top < client.top ||
            control.rect.right > client.right ||
            control.rect.bottom > client.bottom ||
            control.rect.right <= control.rect.left ||
            control.rect.bottom <= control.rect.top) {
            wchar_t text[256]{};
            GetWindowTextW(control.window, text,
                           static_cast<int>(std::size(text)));
            std::fprintf(stderr,
                         "dashboard smoke: control outside client class=%ls "
                         "text=%ls client=[%ld,%ld,%ld,%ld] "
                         "rect=[%ld,%ld,%ld,%ld]\n",
                         control.class_name.c_str(), text, client.left,
                         client.top, client.right, client.bottom,
                         control.rect.left, control.rect.top, control.rect.right,
                         control.rect.bottom);
            return false;
        }
        const auto style = GetWindowLongPtrW(control.window, GWL_STYLE);
        if (class_name_is(control.class_name.c_str(), L"COMBOBOX") &&
            (style & CBS_OWNERDRAWFIXED) != 0) {
            has_owner_drawn_combo = true;
        }
        if (class_name_is(control.class_name.c_str(), L"BUTTON")) {
            wchar_t text[256]{};
            GetWindowTextW(control.window, text,
                           static_cast<int>(std::size(text)));
            const auto type = style & BS_TYPEMASK;
            if (type == BS_OWNERDRAW && std::wstring_view{text} ==
                                            L"Generate audio") {
                has_owner_drawn_checkbox = true;
            }
        }
    }

    for (std::size_t first = 0; first < collection.controls.size(); ++first) {
        for (std::size_t second = first + 1;
             second < collection.controls.size(); ++second) {
            if (rectangles_overlap(collection.controls[first].rect,
                                   collection.controls[second].rect)) {
                const auto& first_control = collection.controls[first];
                const auto& second_control = collection.controls[second];
                std::fprintf(
                    stderr,
                    "dashboard smoke: overlapping controls %ls "
                    "[%ld,%ld,%ld,%ld] and %ls [%ld,%ld,%ld,%ld]\n",
                    first_control.class_name.c_str(), first_control.rect.left,
                    first_control.rect.top, first_control.rect.right,
                    first_control.rect.bottom, second_control.class_name.c_str(),
                    second_control.rect.left, second_control.rect.top,
                    second_control.rect.right, second_control.rect.bottom);
                return false;
            }
        }
    }
    if (!has_owner_drawn_checkbox || !has_owner_drawn_combo) {
        std::fprintf(stderr,
                     "dashboard smoke: interactive controls=%zu "
                     "owner-checkbox=%d owner-combo=%d\n",
                     collection.controls.size(), has_owner_drawn_checkbox,
                     has_owner_drawn_combo);
        for (const auto& control : collection.controls) {
            wchar_t text[256]{};
            GetWindowTextW(control.window, text,
                           static_cast<int>(std::size(text)));
            std::fprintf(stderr,
                         "dashboard smoke: control class=%ls text=%ls "
                         "style=%llx rect=[%ld,%ld,%ld,%ld]\n",
                         control.class_name.c_str(), text,
                         static_cast<unsigned long long>(
                             GetWindowLongPtrW(control.window, GWL_STYLE)),
                         control.rect.left, control.rect.top, control.rect.right,
                         control.rect.bottom);
        }
    }
    return has_owner_drawn_checkbox && has_owner_drawn_combo;
}

bool check_rendered_dashboard(HWND dashboard) {
    RECT client{};
    if (!GetClientRect(dashboard, &client)) return false;
    const auto width = client.right - client.left;
    const auto height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return false;
    const auto window_dc = GetDC(dashboard);
    if (window_dc == nullptr) return false;
    const auto memory_dc = CreateCompatibleDC(window_dc);
    const auto bitmap = CreateCompatibleBitmap(window_dc, width, height);
    if (memory_dc == nullptr || bitmap == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memory_dc != nullptr) DeleteDC(memory_dc);
        ReleaseDC(dashboard, window_dc);
        return false;
    }
    const auto previous = SelectObject(memory_dc, bitmap);
    auto painted = PrintWindow(dashboard, memory_dc, PW_CLIENTONLY);
    if (!painted) {
        // PrintWindow is not implemented by every Windows runner/session.
        // Ask the dashboard to paint the same client area directly before
        // treating the render probe as unavailable.
        painted = SendMessageW(
                      dashboard, WM_PRINT, reinterpret_cast<WPARAM>(memory_dc),
                      PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND) != 0;
    }
    SelectObject(memory_dc, previous);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) *
                                     static_cast<std::size_t>(height) * 4U);
    const auto copied = GetDIBits(memory_dc, bitmap, 0,
                                  static_cast<UINT>(height), pixels.data(),
                                  &info, DIB_RGB_COLORS);
    auto rendered = painted && copied == static_cast<UINT>(height) &&
                    std::any_of(pixels.begin(), pixels.end(),
                                [](const std::uint8_t value) {
                                    return value != 0;
                                });
    if (!rendered) {
        // A minimized or compositor-backed window may decline both print
        // messages while still exposing its client pixels through its DC.
        const auto selected = SelectObject(memory_dc, bitmap);
        const auto blitted = BitBlt(memory_dc, 0, 0, width, height, window_dc,
                                    0, 0, SRCCOPY);
        SelectObject(memory_dc, selected);
        if (blitted) {
            const auto fallback_copied = GetDIBits(
                memory_dc, bitmap, 0, static_cast<UINT>(height), pixels.data(),
                &info, DIB_RGB_COLORS);
            rendered = fallback_copied == static_cast<UINT>(height) &&
                       std::any_of(pixels.begin(), pixels.end(),
                                   [](const std::uint8_t value) {
                                       return value != 0;
                                   });
        }
    }
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(dashboard, window_dc);
    return rendered;
}

DashboardResult invoke_dashboard(const bool can_resume,
                                 DashboardInvocation& invocation) {
    KeyboardBindings keyboard{};
    ActionBindings actions{};
    DashboardLinkSettings link_settings;
    gbb::PluginDiscoveryOptions plugin_options;
    gbb::PluginCatalog plugin_catalog;
    return show_windows_dashboard(
        nullptr, invocation.library, can_resume, 0,
        gbb::CoreCapability::none, 0, gameboy::default_video_mode,
        gameboy::HardwareModel::automatic, true, keyboard, actions,
        link_settings, plugin_options, plugin_catalog, {}, {});
}

bool run_dashboard_case(const bool can_resume, const bool discard,
                        const bool inspect_controls) {
    std::fprintf(stderr, "dashboard smoke: case resume=%d discard=%d inspect=%d\n",
                 can_resume, discard, inspect_controls);
    DashboardInvocation invocation;
    std::thread worker([&] {
        const auto result = invoke_dashboard(can_resume, invocation);
        std::lock_guard lock{invocation.mutex};
        invocation.result = result;
        invocation.completed = true;
    });

    auto dashboard = HWND{};
    const auto opened = wait_for([&] {
        dashboard = dashboard_window();
        return dashboard != nullptr;
    });
    if (!opened) {
        std::fprintf(stderr, "dashboard smoke: dashboard did not open\n");
        worker.join();
        return false;
    }

    auto settings = HWND{};
    bool passed = wait_for([&] {
        settings = child_by_text(dashboard, L"Settings");
        return settings != nullptr;
    });
    if (passed) {
        SendMessageW(settings, BM_CLICK, 0, 0);
        passed = wait_for([&] {
            const auto apply = child_by_text(dashboard, L"Apply and return");
            return apply != nullptr && IsWindowVisible(apply) != FALSE;
        }, std::chrono::milliseconds{250});
        if (!passed) {
            // Owner-drawn buttons on some Windows runners ignore BM_CLICK.
            // Deliver the same notification directly to the dashboard and
            // then wait for the page's visible state.
            constexpr WORD settings_command_id = 101;
            SendMessageW(
                dashboard, WM_COMMAND,
                MAKEWPARAM(settings_command_id, BN_CLICKED),
                reinterpret_cast<LPARAM>(settings));
            passed = wait_for([&] {
                const auto apply =
                    child_by_text(dashboard, L"Apply and return");
                return apply != nullptr && IsWindowVisible(apply) != FALSE;
            });
        }
        if (!passed) {
            std::fprintf(stderr,
                         "dashboard smoke: Settings=%p visible=%d "
                         "Apply=%p visible=%d\n",
                         static_cast<void*>(settings),
                         settings != nullptr && IsWindowVisible(settings),
                         static_cast<void*>(child_by_text(
                             dashboard, L"Apply and return")),
                         child_by_text(dashboard, L"Apply and return") != nullptr &&
                             IsWindowVisible(child_by_text(
                                 dashboard, L"Apply and return")));
        }
    }
    if (passed && inspect_controls) {
        const auto controls_ok = check_native_controls_and_layout(dashboard);
        const auto render_ok = check_rendered_dashboard(dashboard);
        passed = controls_ok && render_ok;
        if (!passed) {
            std::fprintf(stderr,
                         "dashboard smoke: controls=%d render=%d\n",
                         controls_ok, render_ok);
        }
    }
    if (passed && discard) {
        passed = click_child(dashboard, L"Generate audio");
        if (passed) {
            PostMessageW(dashboard, WM_KEYDOWN, VK_ESCAPE, 0);
            passed = confirm_message_box(L"Unsaved settings", IDYES);
        }
    } else if (passed) {
        passed = click_child(dashboard, L"Apply and return");
    }
    if (!passed) close_dashboard(dashboard);
    if (!wait_for([&] { return dashboard_completed(invocation); })) {
        close_dashboard(dashboard_window());
    }
    worker.join();
    std::lock_guard lock{invocation.mutex};
    const auto expected = can_resume ? DashboardResultAction::resume
                                     : DashboardResultAction::library;
    const auto result_matches = invocation.result.action == expected;
    if (!passed || !result_matches) {
        std::fprintf(stderr,
                     "dashboard smoke: case failed passed=%d result=%d expected=%d\n",
                     passed, static_cast<int>(invocation.result.action),
                     static_cast<int>(expected));
    }
    return passed && result_matches;
}

bool check_unreachable_rom_error() {
    const auto path = std::filesystem::temp_directory_path() /
                      "gbb-dashboard-smoke-missing-rom.gb";
    std::error_code error;
    std::filesystem::remove(path, error);
    try {
        static_cast<void>(gbb::built_in_core_registry().create_from_file(path));
    } catch (const std::exception& exception) {
        return std::strlen(exception.what()) != 0;
    } catch (...) {
        return true;
    }
    return false;
}

} // namespace

int run_windows_dashboard_smoke() {
    if (!check_unreachable_rom_error()) return 1;
    if (settings_return_action(true) != DashboardResultAction::resume ||
        settings_return_action(false) != DashboardResultAction::library) {
        return 2;
    }
    if (!run_dashboard_case(false, false, true)) return 3;
    if (!run_dashboard_case(false, true, false)) return 4;
    if (!run_dashboard_case(true, false, false)) return 5;
    if (!run_dashboard_case(true, true, false)) return 6;
    return 0;
}

} // namespace gbb_desktop

#endif
