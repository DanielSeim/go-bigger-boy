#pragma once

#include "sdl_resources.hpp"

#include "gbb/core.hpp"
#include "gameboy/emulator.hpp"

namespace gbb::sdl {

void flush_battery_safely(gbb::EmulatorCore* core) noexcept;
void flush_battery_safely(gameboy::Emulator* emulator) noexcept;
void stop_rumble(SdlResources& sdl) noexcept;
void update_rumble(const gbb::EmulatorCore* core, SdlResources& sdl,
                   bool enabled);

} // namespace gbb::sdl
