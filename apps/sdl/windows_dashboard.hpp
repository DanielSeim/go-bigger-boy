#pragma once

#include "gameboy/rom_library.hpp"

#include <cstddef>
#include <string>

#ifdef _WIN32
#include <windows.h>

namespace gbb_desktop {

enum class DashboardResultAction { resume, open_rom, quit };

struct DashboardResult {
    DashboardResultAction action{DashboardResultAction::resume};
    std::string rom_path;
    std::size_t palette{};
    bool palette_changed{};
};

DashboardResult show_windows_dashboard(
    HWND owner, const gameboy::RomLibrary& library, bool can_resume,
    std::size_t palette, const std::filesystem::path& preference_directory);

} // namespace gbb_desktop
#endif
