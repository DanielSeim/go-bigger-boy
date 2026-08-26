#pragma once

#include "gbb/core.hpp"

namespace gameboy {
class Emulator;
}

namespace gbb {

// Optional GB/GBC development access. Generic frontend paths must stay on
// EmulatorCore; debugger, sprite, printer and GameShark tools may use this
// adapter after checking the relevant capability.
[[nodiscard]] gameboy::Emulator* gameboy_emulator(EmulatorCore* core) noexcept;
[[nodiscard]] const gameboy::Emulator* gameboy_emulator(
    const EmulatorCore* core) noexcept;

} // namespace gbb
