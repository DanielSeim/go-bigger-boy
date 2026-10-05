#pragma once

#include "gameboy/hardware_model.hpp"

#include <array>
#include <cstdint>

namespace gameboy {

enum class BootRomMode {
    post_boot,
    diagnostic,
    replacement_dmg,
    // Original-cadence firmware, with a host-rendered GBB splash and chime.
    animated_dmg,
    // Legacy DMG mode names select the corresponding hardware's image.
    replacement_mgb = replacement_dmg,
    animated_mgb = animated_dmg,
    replacement_dmg0 = replacement_dmg,
    animated_dmg0 = animated_dmg,
    replacement_cgb = replacement_dmg,
    animated_cgb = animated_dmg,
};

constexpr std::size_t diagnostic_boot_rom_size = 0x100;
using DiagnosticBootRom = std::array<std::uint8_t, diagnostic_boot_rom_size>;

// This is intentionally a small, original diagnostic ROM rather than a copy
// of Nintendo's boot ROM. It establishes the documented CPU handoff state,
// writes a marker to HRAM, disables the mapped ROM at FF50, and falls through
// to the cartridge entry point at 0100.
[[nodiscard]] DiagnosticBootRom diagnostic_boot_rom(HardwareModel model) noexcept;

// Original cold-start firmware for the monochrome and color profiles.
// Unlike the diagnostic ROM, this image initializes peripherals itself.
[[nodiscard]] const DiagnosticBootRom& dmg_boot_rom() noexcept;
[[nodiscard]] const DiagnosticBootRom& dmg0_boot_rom() noexcept;
[[nodiscard]] const DiagnosticBootRom& dmg0_animated_boot_rom() noexcept;
[[nodiscard]] const DiagnosticBootRom& mgb_boot_rom() noexcept;
[[nodiscard]] const DiagnosticBootRom& dmg_animated_boot_rom() noexcept;
[[nodiscard]] const DiagnosticBootRom& mgb_animated_boot_rom() noexcept;
[[nodiscard]] const DiagnosticBootRom& cgb_boot_rom() noexcept;
[[nodiscard]] const DiagnosticBootRom& cgb_animated_boot_rom() noexcept;
[[nodiscard]] const DiagnosticBootRom& cgb0_boot_rom() noexcept;
[[nodiscard]] const DiagnosticBootRom& cgb0_animated_boot_rom() noexcept;

} // namespace gameboy
