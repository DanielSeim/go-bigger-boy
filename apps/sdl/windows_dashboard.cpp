#include "windows_dashboard.hpp"

#ifdef _WIN32

#include <commctrl.h>
#include <commdlg.h>

#include <array>
#include <filesystem>
#include <string>
#include <utility>

namespace gbb_desktop {
namespace {

constexpr int id_library = 100;
constexpr int id_settings = 101;
constexpr int id_list = 102;
constexpr int id_open = 103;
constexpr int id_play = 104;
constexpr int id_resume = 105;
constexpr int id_quit = 106;
constexpr int id_palette = 107;

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

struct State {
    const gameboy::RomLibrary* library{};
    DashboardResult result;
    bool can_resume{};
    bool done{};
    HWND window{};
    HWND list{};
    HWND play{};
    HWND open{};
    HWND resume{};
    HWND quit{};
    HWND palette{};
    HWND settings_heading{};
    HWND palette_label{};
    HFONT title_font{};
};

void show_page(State& state, const bool settings) {
    ShowWindow(state.list, settings ? SW_HIDE : SW_SHOW);
    ShowWindow(state.play, settings ? SW_HIDE : SW_SHOW);
    ShowWindow(state.open, settings ? SW_HIDE : SW_SHOW);
    ShowWindow(state.resume, settings || !state.can_resume ? SW_HIDE : SW_SHOW);
    ShowWindow(state.settings_heading, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.palette_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.palette, settings ? SW_SHOW : SW_HIDE);
}

void finish(State& state, const DashboardResultAction action,
            const std::string& path = {}) {
    state.result.action = action;
    state.result.rom_path = path;
    state.done = true;
    DestroyWindow(state.window);
}

void play_selection(State& state) {
    const auto selected = ListView_GetNextItem(state.list, -1, LVNI_SELECTED);
    if (selected < 0 ||
        static_cast<std::size_t>(selected) >= state.library->entries().size()) {
        return;
    }
    finish(state, DashboardResultAction::open_rom,
           state.library->entries()[static_cast<std::size_t>(selected)]
               .path.u8string());
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

    if (message == WM_COMMAND) {
        switch (LOWORD(wparam)) {
        case id_library: show_page(*state, false); return 0;
        case id_settings: show_page(*state, true); return 0;
        case id_open: open_rom(*state); return 0;
        case id_play: play_selection(*state); return 0;
        case id_resume: finish(*state, DashboardResultAction::resume); return 0;
        case id_quit: finish(*state, DashboardResultAction::quit); return 0;
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
        default: break;
        }
    } else if (message == WM_NOTIFY) {
        const auto* notification = reinterpret_cast<NMHDR*>(lparam);
        if (notification->idFrom == id_list && notification->code == NM_DBLCLK) {
            play_selection(*state);
            return 0;
        }
    } else if (message == WM_CLOSE) {
        finish(*state, state->can_resume ? DashboardResultAction::resume
                                         : DashboardResultAction::quit);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND control(State& state, const wchar_t* type, const wchar_t* text,
             DWORD style, int x, int y, int width, int height, int id) {
    auto result = CreateWindowExW(0, type, text,
        WS_CHILD | style, x, y, width, height, state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(result, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return result;
}

} // namespace

DashboardResult show_windows_dashboard(
    HWND owner, const gameboy::RomLibrary& library, const bool can_resume,
    const std::size_t palette) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    const auto instance = GetModuleHandleW(nullptr);
    constexpr auto class_name = L"GoBiggerBoyDashboard";
    WNDCLASSW type{};
    type.lpfnWndProc = window_proc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    type.lpszClassName = class_name;
    RegisterClassW(&type);

    State state;
    state.library = &library;
    state.can_resume = can_resume;
    state.result.palette = palette;
    state.window = CreateWindowExW(
        WS_EX_APPWINDOW, class_name, L"Go Bigger Boy — Game Library",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 570, owner, nullptr, instance, &state);
    if (state.window == nullptr) {
        state.result.action = can_resume ? DashboardResultAction::resume
                                         : DashboardResultAction::quit;
        return state.result;
    }

    control(state, L"STATIC", L"Go Bigger Boy", WS_VISIBLE, 24, 20, 300, 34, 0);
    control(state, L"BUTTON", L"Library", WS_VISIBLE | BS_PUSHBUTTON,
            24, 68, 110, 34, id_library);
    control(state, L"BUTTON", L"Settings", WS_VISIBLE | BS_PUSHBUTTON,
            142, 68, 110, 34, id_settings);
    state.list = control(state, WC_LISTVIEWW, L"",
        WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        24, 118, 750, 320, id_list);
    ListView_SetExtendedListViewStyle(state.list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    constexpr std::array<std::pair<const wchar_t*, int>, 3> columns{{
        {L"Game", 390}, {L"Platform", 170}, {L"Language", 165}}};
    for (std::size_t index = 0; index < columns.size(); ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(columns[index].first);
        column.cx = columns[index].second;
        ListView_InsertColumn(state.list, static_cast<int>(index), &column);
    }
    for (std::size_t index = 0; index < library.entries().size(); ++index) {
        const auto& entry = library.entries()[index];
        auto title = widen(entry.metadata.title);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = title.data();
        ListView_InsertItem(state.list, &item);
        auto platform_name = widen(gameboy::platform_name(entry.metadata.platform));
        LVITEMW subitem{};
        subitem.iSubItem = 1;
        subitem.pszText = platform_name.data();
        SendMessageW(state.list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&subitem));
        auto language = widen(entry.metadata.language);
        subitem.iSubItem = 2;
        subitem.pszText = language.data();
        SendMessageW(state.list, LVM_SETITEMTEXTW,
                     static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&subitem));
    }
    state.open = control(state, L"BUTTON", L"Open ROM…",
        WS_VISIBLE | BS_PUSHBUTTON, 24, 458, 130, 38, id_open);
    state.play = control(state, L"BUTTON", L"Play selected",
        WS_VISIBLE | BS_DEFPUSHBUTTON, 164, 458, 140, 38, id_play);
    state.resume = control(state, L"BUTTON", L"Resume game",
        (can_resume ? WS_VISIBLE : 0) | BS_PUSHBUTTON,
        314, 458, 140, 38, id_resume);
    state.quit = control(state, L"BUTTON", L"Quit",
        WS_VISIBLE | BS_PUSHBUTTON, 664, 458, 110, 38, id_quit);

    state.settings_heading = control(state, L"STATIC", L"Display settings",
        0, 24, 132, 300, 28, 0);
    state.palette_label = control(state, L"STATIC", L"Color palette",
        0, 24, 184, 180, 24, 0);
    state.palette = control(state, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL, 24, 214, 320, 180, id_palette);
    constexpr std::array<const wchar_t*, 5> palettes{{
        L"Grayscale", L"Classic green", L"Game Boy Pocket", L"Amber",
        L"Game Boy Color (automatic)"}};
    for (const auto* name : palettes) {
        SendMessageW(state.palette, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name));
    }
    SendMessageW(state.palette, CB_SETCURSEL,
                 static_cast<WPARAM>(palette), 0);
    show_page(state, false);

    if (owner != nullptr) EnableWindow(owner, FALSE);
    ShowWindow(state.window, SW_SHOW);
    UpdateWindow(state.window);
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (owner != nullptr) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return state.result;
}

} // namespace gbb_desktop
#endif
