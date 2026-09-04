#pragma once

#include "gameboy/rom_library.hpp"
#include "gameboy/video_pipeline.hpp"
#include "gbb/voxel_profile.hpp"
#include "gbb/core.hpp"

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
using ActionBindings = std::array<std::int64_t, 4>;

struct DashboardResult {
    DashboardResultAction action{DashboardResultAction::resume};
    std::string rom_path;
    std::size_t palette{};
    bool palette_changed{};
    gameboy::VideoMode video_mode{gameboy::default_video_mode};
    bool video_mode_changed{};
    KeyboardBindings keyboard_bindings{};
    bool keyboard_bindings_changed{};
    ActionBindings action_bindings{};
    bool action_bindings_changed{};
    bool voxel_profile_changed{};
    std::vector<std::uint64_t> removed_fingerprints;
};

DashboardResult show_windows_dashboard(
    HWND owner, const gameboy::RomLibrary& library, bool can_resume,
    std::uint64_t current_fingerprint,
    gbb::CoreCapability capabilities,
    std::size_t palette, gameboy::VideoMode video_mode,
    const KeyboardBindings& keyboard_bindings,
    const ActionBindings& action_bindings,
    const std::filesystem::path& preference_directory);

} // namespace gbb_desktop
#endif
