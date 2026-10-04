#pragma once

#include "gameboy/ppu.hpp"
#include <cstdint>

namespace gameboy {
// Original presentation only: neither function reads or modifies emulated RAM/APU.
inline constexpr unsigned boot_splash_frame_cycles = 70224;
[[nodiscard]] std::uint64_t boot_splash_sample_time(std::uint64_t cycles) noexcept;
void render_boot_splash(Ppu::Framebuffer& pixels, std::uint64_t frame) noexcept;
[[nodiscard]] std::int16_t boot_splash_sample(std::uint64_t sample) noexcept;
}
