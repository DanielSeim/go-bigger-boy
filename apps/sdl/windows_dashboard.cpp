#include "windows_dashboard.hpp"
#include "resource.h"
#include "update_checker.hpp"

#ifdef _WIN32

#include <commctrl.h>
#include <commdlg.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
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
constexpr int id_remove = 108;
constexpr UINT artwork_ready = WM_APP + 1;

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
    bool done{};
    HWND window{};
    HWND list{};
    HWND play{};
    HWND open{};
    HWND resume{};
    HWND quit{};
    HWND remove{};
    HWND palette{};
    HWND settings_heading{};
    HWND palette_label{};
    HWND logo{};
    HBITMAP logo_bitmap{};
    HIMAGELIST covers{};
    std::filesystem::path preference_directory;
    std::thread artwork_worker;
    std::atomic_bool closing{};
    HFONT title_font{};
};

void show_page(State& state, const bool settings) {
    ShowWindow(state.list, settings ? SW_HIDE : SW_SHOW);
    ShowWindow(state.play, settings ? SW_HIDE : SW_SHOW);
    ShowWindow(state.open, settings ? SW_HIDE : SW_SHOW);
    ShowWindow(state.remove, settings ? SW_HIDE : SW_SHOW);
    ShowWindow(state.resume, settings || !state.can_resume ? SW_HIDE : SW_SHOW);
    ShowWindow(state.settings_heading, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.palette_label, settings ? SW_SHOW : SW_HIDE);
    ShowWindow(state.palette, settings ? SW_SHOW : SW_HIDE);
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

void finish(State& state, const DashboardResultAction action,
            const std::string& path = {}) {
    state.result.action = action;
    state.result.rom_path = path;
    state.closing = true;
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
        case id_remove: remove_selection(*state); return 0;
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
        finish(*state, state->can_resume ? DashboardResultAction::resume
                                         : DashboardResultAction::quit);
        return 0;
    } else if (message == WM_DESTROY) {
        if (state->logo_bitmap != nullptr) {
            DeleteObject(state->logo_bitmap);
            state->logo_bitmap = nullptr;
        }
        if (state->covers != nullptr) {
            ImageList_Destroy(state->covers);
            state->covers = nullptr;
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
    const std::filesystem::path& directory, const std::string& system) {
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
                                               error));
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
                system, load_database(state.preference_directory, system)).first;
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
            static_cast<void>(download_public_file(url, cover, 5 * 1024 * 1024,
                                                   error));
        }
        auto update = std::make_unique<ArtworkUpdate>(ArtworkUpdate{
            index, std::move(title), std::move(language),
            std::filesystem::is_regular_file(cover)
                ? cover
                : std::filesystem::path{}});
        if (!PostMessageW(state.window, artwork_ready, 0,
                          reinterpret_cast<LPARAM>(update.get()))) {
            return;
        }
        static_cast<void>(update.release());
    }
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
    const std::size_t palette,
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
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    type.lpszClassName = class_name;
    RegisterClassW(&type);

    State state;
    state.library = &library;
    state.can_resume = can_resume;
    state.result.palette = palette;
    state.preference_directory = preference_directory;
    state.window = CreateWindowExW(
        WS_EX_APPWINDOW, class_name, L"Go Bigger Boy - Game Library",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 620, owner, nullptr, instance, &state);
    if (state.window == nullptr) {
        state.result.action = can_resume ? DashboardResultAction::resume
                                         : DashboardResultAction::quit;
        return state.result;
    }

    SendMessageW(state.window, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(type.hIcon));
    SendMessageW(state.window, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(type.hIcon));
    state.logo_bitmap = load_logo_bitmap(instance, 270, 90);
    state.logo = control(state, L"STATIC", L"Go Bigger Boy",
        WS_VISIBLE | (state.logo_bitmap != nullptr ? SS_BITMAP : 0),
        24, 12, 270, 90, 0);
    if (state.logo_bitmap != nullptr) {
        SendMessageW(state.logo, STM_SETIMAGE, IMAGE_BITMAP,
                     reinterpret_cast<LPARAM>(state.logo_bitmap));
    }
    control(state, L"BUTTON", L"Library", WS_VISIBLE | BS_PUSHBUTTON,
            24, 112, 110, 34, id_library);
    control(state, L"BUTTON", L"Settings", WS_VISIBLE | BS_PUSHBUTTON,
            142, 112, 110, 34, id_settings);
    state.list = control(state, WC_LISTVIEWW, L"",
        WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        24, 158, 750, 340, id_list);
    ListView_SetExtendedListViewStyle(state.list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
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
        {L"Cover", 56}, {L"Game", 224}, {L"Platform", 116},
        {L"Language", 106}, {L"Last played", 210}}};
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
        WS_VISIBLE | BS_PUSHBUTTON, 24, 515, 130, 38, id_open);
    state.play = control(state, L"BUTTON", L"Play selected",
        WS_VISIBLE | BS_DEFPUSHBUTTON, 164, 515, 140, 38, id_play);
    state.remove = control(state, L"BUTTON", L"Remove from list",
        WS_VISIBLE | BS_PUSHBUTTON, 314, 515, 140, 38, id_remove);
    state.resume = control(state, L"BUTTON", L"Resume game",
        (can_resume ? WS_VISIBLE : 0) | BS_PUSHBUTTON,
        464, 515, 130, 38, id_resume);
    state.quit = control(state, L"BUTTON", L"Quit",
        WS_VISIBLE | BS_PUSHBUTTON, 664, 515, 110, 38, id_quit);

    state.settings_heading = control(state, L"STATIC", L"Display settings",
        0, 24, 174, 300, 28, 0);
    state.palette_label = control(state, L"STATIC", L"Color palette",
        0, 24, 220, 180, 24, 0);
    state.palette = control(state, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL, 24, 250, 320, 180, id_palette);
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
    state.artwork_worker = std::thread([&state] { resolve_artwork(state); });
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
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
