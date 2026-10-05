// SPDX-License-Identifier: GPL-3.0-or-later
// Render original GBB intro frames for visual review, without a cartridge.
#include "gameboy/boot_splash.hpp"
#include <cstdio>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    auto pixels = std::make_unique<gameboy::Ppu::Framebuffer>();
    for (unsigned frame : {0U, 40U, 60U, 80U, 90U, 120U, 160U, 177U}) {
        gameboy::render_boot_splash(*pixels, frame, gameboy::HardwareModel::cgb);
        const auto path = std::string(argv[1]) + "/frame-" + std::to_string(frame) + ".ppm";
        auto* out = std::fopen(path.c_str(), "wbx");
        if (!out) return 3;
        std::fprintf(out, "P6\n160 144\n255\n");
        for (auto pixel : *pixels) {
            const unsigned char rgb[] = {static_cast<unsigned char>(pixel >> 16),
                static_cast<unsigned char>(pixel >> 8), static_cast<unsigned char>(pixel)};
            if (std::fwrite(rgb, 1, 3, out) != 3) { std::fclose(out); return 4; }
        }
        if (std::fclose(out)) return 4;
    }
}
