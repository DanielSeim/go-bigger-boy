#pragma once

#include "gameboy/ppu.hpp"
#include "gameboy/hardware_model.hpp"
#include <cstdint>

namespace gameboy {
// Original presentation only: neither function reads or modifies emulated RAM/APU.
inline constexpr unsigned boot_splash_frame_cycles = 70224;
inline constexpr std::uint64_t boot_splash_delay_cycles = 19'136'512;
inline constexpr std::uint64_t boot_splash_first_note_cycle = 17'467'324;
inline constexpr std::uint64_t boot_splash_second_note_cycle = 17'818'480;
[[nodiscard]] std::uint64_t boot_splash_sample_time(std::uint64_t cycles) noexcept;
void prepare_boot_splash_audio(HardwareModel model = HardwareModel::dmg);
void render_boot_splash(Ppu::Framebuffer& pixels, std::uint64_t frame, HardwareModel model = HardwareModel::dmg) noexcept;
[[nodiscard]] std::int16_t boot_splash_sample(std::uint64_t sample, HardwareModel model = HardwareModel::dmg);
}
