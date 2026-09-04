#pragma once

#ifndef __ANDROID__

#include "gameboy/gameshark.hpp"
#include "gameboy/rom_library.hpp"

#include "cheat_manager.hpp"
#include "core_capability.hpp"
#include "desktop_debugger.hpp"
#include "input_movie.hpp"
#include "sdl_resources.hpp"
#include "sprite_editor.hpp"
#include "tas_editor.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace gbb::sdl {

// References keep this boundary orchestration-only: ownership of the tools
// and emulator remains with the SDL session in main.cpp.
struct AdvancedToolContext final {
    CoreServices services;
    SdlResources& sdl;
    DesktopDebugger& debugger;
    InputMovie& input_movie;
    TasEditor& tas_editor;
    SpriteEditor& sprite_editor;
    CheatManager& cheat_manager;
    const std::filesystem::path& movie_path;
    const std::filesystem::path& sprite_patch_path;
    const std::filesystem::path& sprite_ips_path;
    const std::string& current_rom;
    std::deque<std::vector<std::uint8_t>>& rewind_history;
    bool& paused;
    bool& fast_forward;
    bool& rewind;
};

// Consume pending desktop debugger/editor requests for one frontend frame.
// Capability checks are centralized here so adding a core cannot accidentally
// route a request to an incompatible concrete adapter.
void process_advanced_tool_requests(AdvancedToolContext context);

} // namespace gbb::sdl

#endif // __ANDROID__
