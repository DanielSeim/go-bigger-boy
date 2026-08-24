#pragma once

#include "gameboy/rom_library.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

namespace gbb_desktop {

enum class DashboardResultAction { resume, open_rom, quit };

using KeyboardBindings = std::array<std::array<std::int64_t, 2>, 8>;

struct DashboardResult {
    DashboardResultAction action{DashboardResultAction::resume};
    std::string rom_path;
    std::size_t palette{};
    bool palette_changed{};
    KeyboardBindings keyboard_bindings{};
    bool keyboard_bindings_changed{};
    std::vector<std::uint64_t> removed_fingerprints;
};

DashboardResult show_windows_dashboard(
    HWND owner, const gameboy::RomLibrary& library, bool can_resume,
    std::size_t palette, const KeyboardBindings& keyboard_bindings,
    const std::filesystem::path& preference_directory);

} // namespace gbb_desktop
#endif
