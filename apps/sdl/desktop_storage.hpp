#pragma once

#include "gameboy/emulator.hpp"
#include "gameboy/rom_library.hpp"
#include "gbb/core.hpp"
#include "gbb/log.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gbb::sdl {

struct DialogState {
    std::mutex mutex;
    bool active{};
    std::optional<std::string> selected_path;
    std::optional<std::string> error;
    // SDL invokes the file-dialog callback asynchronously (and potentially
    // from a different thread). Keep the initiating frame/ROM metadata with
    // the request so callback diagnostics can be correlated with the UI
    // action that opened it.
    gbb::LogContext log_context{};
};

void show_rom_dialog(DialogState& state, SDL_Window* window);
[[nodiscard]] bool dialog_active(DialogState& state);
void collect_dialog_result(DialogState& state,
                           std::optional<std::string>& path,
                           std::optional<std::string>& error);

struct WindowGeometry {
    int x{};
    int y{};
    int width{};
    int height{};
};

[[nodiscard]] std::filesystem::path preference_directory();
void restore_game_window_geometry(SDL_Window* window,
                                  const std::filesystem::path& directory);
void save_game_window_geometry(SDL_Window* window,
                               const std::filesystem::path& directory);

[[nodiscard]] std::vector<std::string>
recent_paths(const gameboy::RomLibrary& library);
[[nodiscard]] gameboy::RomLibrary
load_rom_library(const std::filesystem::path& directory);

[[nodiscard]] std::filesystem::path
quick_state_path(const std::filesystem::path& preference_path,
                 const gbb::EmulatorCore& core);
void save_quick_state(const std::filesystem::path& preference_path,
                      const gbb::EmulatorCore& core);
void load_quick_state(const std::filesystem::path& preference_path,
                      gbb::EmulatorCore& core);

void save_completed_prints(gbb::EmulatorCore* core, SDL_Window* window,
                           const std::filesystem::path& preference_path,
                           const std::string& current_rom,
                           std::uint64_t& print_sequence);

} // namespace gbb::sdl
