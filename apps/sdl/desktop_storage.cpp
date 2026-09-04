#include "desktop_storage.hpp"

#include "gbb/frontend_logging.hpp"

#include <chrono>
#include <cerrno>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gbb::sdl {
namespace {

constexpr std::uintmax_t maximum_quick_state_size = 2 * 1024 * 1024;

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

} // namespace

std::filesystem::path preference_directory() {
#ifdef _WIN32
    const auto* raw_path = SDL_GetBasePath();
    if (raw_path == nullptr) return {};
    const auto path = std::filesystem::u8path(raw_path);
    return path.lexically_normal();
#else
    char* raw_path = SDL_GetPrefPath("Go Bigger Boy", "GBB");
    if (raw_path == nullptr) return {};
    const auto path = std::filesystem::u8path(raw_path);
    SDL_free(raw_path);
    return path;
#endif
}

void restore_game_window_geometry(
    SDL_Window* window, const std::filesystem::path& directory) {
    if (directory.empty() || window == nullptr) return;
    std::ifstream input(directory / "game-window.txt");
    WindowGeometry geometry;
    if (!(input >> geometry.x >> geometry.y >> geometry.width >>
          geometry.height) ||
        geometry.width < 320 || geometry.height < 288 ||
        !geometry_is_visible(geometry)) {
        return;
    }
    static_cast<void>(SDL_SetWindowSize(window, geometry.width, geometry.height));
    static_cast<void>(SDL_SetWindowPosition(window, geometry.x, geometry.y));
}

void save_game_window_geometry(
    SDL_Window* window, const std::filesystem::path& directory) {
    if (directory.empty() || window == nullptr ||
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

std::filesystem::path quick_state_path(
    const std::filesystem::path& preference_path,
    const gbb::EmulatorCore& core) {
    if (preference_path.empty()) {
        throw std::runtime_error("Could not locate the preferences directory");
    }
    std::ostringstream name;
    name << std::hex << std::setw(16) << std::setfill('0')
         << core.rom_fingerprint() << ".gbbs";
    return preference_path / "states" / name.str();
}

void save_completed_prints(gbb::EmulatorCore* core, SDL_Window* window,
                           const std::filesystem::path& preference_path,
                           const std::string& current_rom,
                           std::uint64_t& print_sequence) {
    if (core == nullptr) return;
    auto pages = core->take_printer_pages();
    if (pages.empty()) return;
    const auto directory = preference_path.empty()
                               ? std::filesystem::current_path() / "GBB Prints"
                               : preference_path / "prints";
    std::filesystem::create_directories(directory);
    auto rom_name = std::filesystem::u8path(current_rom).stem().u8string();
    if (rom_name.empty()) rom_name = "gameboy";
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    std::vector<std::filesystem::path> paths;
    paths.reserve(pages.size());
    for (const auto& page : pages) {
        const gameboy::PrinterImage image{page.height, page.pixels};
        const auto filename = rom_name + "-print-" + std::to_string(timestamp) +
                              "-" + std::to_string(++print_sequence) + ".bmp";
        auto path = directory / std::filesystem::u8path(filename);
        save_printer_bitmap(path, image);
        paths.push_back(std::move(path));
    }
    std::ostringstream message;
    message << "Saved " << paths.size() << " printer image";
    if (paths.size() != 1) message << 's';
    message << " to:\n" << directory.u8string();
    const auto text = message.str();
    gbb::log_frontend_info(text);
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_INFORMATION, "Game Boy Printer", text.c_str(), window));
}

void save_quick_state(const std::filesystem::path& preference_path,
                      const gbb::EmulatorCore& core) {
    const auto path = quick_state_path(preference_path, core);
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    try {
        const auto state = core.save_state();
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
                      gbb::EmulatorCore& core) {
    const auto path = quick_state_path(preference_path, core);
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
    core.load_state(state);
}

} // namespace gbb::sdl
