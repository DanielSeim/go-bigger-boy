#pragma once

#include "gameboy/hardware_model.hpp"

#include <array>
#include <cstdint>

namespace gameboy {

enum class BootRomMode {
    post_boot,
    diagnostic,
};

constexpr std::size_t diagnostic_boot_rom_size = 0x100;
using DiagnosticBootRom = std::array<std::uint8_t, diagnostic_boot_rom_size>;

// This is intentionally a small, original diagnostic ROM rather than a copy
// of Nintendo's boot ROM. It establishes the documented CPU handoff state,
// writes a marker to HRAM, disables the mapped ROM at FF50, and falls through
// to the cartridge entry point at 0100.
[[nodiscard]] DiagnosticBootRom diagnostic_boot_rom(HardwareModel model) noexcept;

} // namespace gameboy
