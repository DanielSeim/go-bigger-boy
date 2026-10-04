// SPDX-License-Identifier: GPL-3.0-or-later
// Original lettering and synthesized tones, not extracted firmware/assets.
#include "gameboy/boot_splash.hpp"
#include "gameboy/apu.hpp"
#include <algorithm>
#include <array>
#include <string_view>

namespace gameboy {
namespace {
// Hand-authored 4x7 glyphs. Lowercase stays lowercase, including the descenders.
constexpr std::array<std::array<unsigned, 7>, 8> glyphs{{
    {{6,9,8,11,9,9,6}},  // G
    {{0,0,6,9,9,9,6}},   // o
    {{14,9,9,14,9,9,14}},// B
    {{2,0,6,2,2,2,7}},   // i
    {{0,6,9,9,7,1,6}},   // g
    {{0,0,6,9,15,8,7}},  // e
    {{0,0,11,12,8,8,8}}, // r
    {{0,0,9,9,7,1,6}},   // y
}};
constexpr std::string_view logo = "Go Bigger Boy";
}

std::uint64_t boot_splash_sample_time(const std::uint64_t cycles) noexcept {
    return (cycles / 4194304) * Apu::sample_rate +
           (cycles % 4194304) * Apu::sample_rate / 4194304;
}

void render_boot_splash(Ppu::Framebuffer& pixels, const std::uint64_t frame) noexcept {
    pixels.fill(0xffffffffU);
    const int top = -16 + static_cast<int>(std::min(frame, std::uint64_t{36}) * 81 / 36);
    for (std::size_t character = 0; character < logo.size(); ++character) {
        if (logo[character] == ' ') continue;
        // The lookup intentionally excludes spaces from the glyph table.
        constexpr std::string_view lookup = "GoBigery";
        const auto glyph = lookup.find(logo[character]);
        if (glyph == lookup.npos) continue;
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 4; ++column) {
                if (!(glyphs[glyph][static_cast<std::size_t>(row)] & (8U >> column))) continue;
                for (int dy = 0; dy < 2; ++dy) {
                    const int y = top + row * 2 + dy;
                    if (y < 0 || y >= 144) continue;
                    for (int dx = 0; dx < 2; ++dx) {
                        const auto x = 16 + character * 10 + static_cast<std::size_t>(column * 2 + dx);
                        pixels[static_cast<std::size_t>(y) * 160 + x] = 0xff000000U;
                    }
                }
            }
        }
    }
}

std::int16_t boot_splash_sample(const std::uint64_t sample) noexcept {
    // A short G4/G5 pulse chime, with click-free attack and decaying tails.
    // Frequencies, envelope and timing are original GBB presentation choices.
    for (unsigned note = 0; note < 2; ++note) {
        const std::uint64_t start = Apu::sample_rate * (note ? 790 : 640) / 1000;
        const std::uint64_t length = Apu::sample_rate * (note ? 145 : 85) / 1000;
        if (sample < start || sample >= start + length) continue;
        const auto offset = sample - start;
        const auto attack = std::min(offset + 1, std::uint64_t{96});
        const auto magnitude = 6000 * attack * (length - offset) / (96 * length);
        const bool positive = ((offset * (note ? 784 : 392) * 2 / Apu::sample_rate) & 1) == 0;
        return static_cast<std::int16_t>(positive ? static_cast<int>(magnitude) : -static_cast<int>(magnitude));
    }
    return 0;
}
}
