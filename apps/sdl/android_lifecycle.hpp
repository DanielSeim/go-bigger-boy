#pragma once

#ifdef __ANDROID__

#include "sdl_resources.hpp"

#include "gbb/core_runtime.hpp"

#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gameboy {
class Emulator;
}

namespace gbb::sdl {

void open_android_library(bool return_to_game = true,
                          const std::string& running_rom = {}) noexcept;

void leave_android_game(
    std::unique_ptr<gbb::EmulatorCore>& core, gameboy::Emulator*& emulator,
    SdlResources& sdl, bool& dashboard_visible, bool& paused,
    bool& fast_forward, bool& rewind,
    std::deque<std::vector<std::uint8_t>>& rewind_history, bool& running);

[[nodiscard]] std::string persist_android_rom(
    const std::string& source, const std::filesystem::path& preference_path,
    const std::string& preferred_display_name);

} // namespace gbb::sdl

#endif
