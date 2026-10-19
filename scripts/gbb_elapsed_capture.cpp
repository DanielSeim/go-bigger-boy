// SPDX-License-Identifier: GPL-3.0-or-later
// Research adapter: ROM owned|post TARGETS_8MHZ OUTPUT_PREFIX (native CGB-E).
#include "gameboy/emulator.hpp"
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

static constexpr std::uint64_t clock_limit = 200000000;
static std::vector<std::uint64_t> parse_targets(std::string_view text) {
    std::vector<std::uint64_t> result;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto word = text.substr(0, comma);
        std::uint64_t value{};
        const auto parsed = std::from_chars(word.data(), word.data() + word.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != word.data() + word.size() ||
            !value || value > clock_limit || result.size() == 8 ||
            (!result.empty() && value <= result.back())) return {};
        result.push_back(value);
        if (comma == std::string_view::npos) return result;
        text.remove_prefix(comma + 1);
    }
    return {}; // Empty input or trailing separator.
}

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const std::string mode = argv[2];
    if (mode != "owned" && mode != "post") return 2;
    const auto targets = parse_targets(argv[3]);
    if (targets.empty()) return 2;
    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) return 2;
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input}, {}};
        gameboy::Emulator e{gameboy::Cartridge{std::move(bytes)}, gameboy::HardwareModel::cgb_e,
            mode == "owned" ? gameboy::BootRomMode::replacement : gameboy::BootRomMode::post_boot};
        e.set_dmg_compatibility_colors(false);
        unsigned frames = 0, zeros = 0;
        auto step = [&] {
            const auto before = e.bus().debug_normalized_clocks();
            (void)e.step();
            const auto after = e.bus().debug_normalized_clocks();
            if (after == before && ++zeros > 1024) throw std::runtime_error("stalled clock");
            if (after > before) zeros = 0;
            if (e.frame_ready()) { ++frames; e.consume_frame(); (void)e.take_audio_samples(); }
        };
        while (e.bus().boot_rom_enabled()) {
            step();
            if (e.bus().debug_normalized_clocks() > clock_limit)
                throw std::runtime_error("boot clock limit");
        }
        if (e.cpu().registers().pc != 0x100) throw std::runtime_error("invalid handoff");
        const auto origin = e.bus().debug_normalized_clocks();
        const auto& r = e.cpu().registers();
        std::printf("{\"event\":\"handoff\",\"model\":\"cgb-e\",\"boot_mode\":\"%s\","
                    "\"boot_clocks_8mhz\":%llu,\"pc\":%u,\"af\":%u,\"bc\":%u,"
                    "\"de\":%u,\"hl\":%u,\"sp\":%u,\"div\":%u,\"lcdc\":%u,\"ly\":%u}\n",
                    mode.c_str(), static_cast<unsigned long long>(origin), r.pc, (r.a<<8)|r.f,
                    (r.b<<8)|r.c, (r.d<<8)|r.e, (r.h<<8)|r.l, r.sp,
                    e.bus().read8(0xff04), e.bus().read8(0xff40), e.bus().read8(0xff44));
        frames = 0;
        for (const auto target : targets) {
            while (e.bus().debug_normalized_clocks() - origin < target) step();
            const auto path = std::string(argv[4]) + "-" + std::to_string(target) + ".ppm";
            // Exclusive creation protects earlier evidence from accidental overwrite.
            FILE* file = std::fopen(path.c_str(), "wbx");
            if (!file) throw std::runtime_error("cannot create capture");
            bool ok = std::fprintf(file, "P6\n160 144\n255\n") > 0;
            for (const auto color : e.framebuffer()) {
                const unsigned char pixel[]{static_cast<unsigned char>(color>>16),
                    static_cast<unsigned char>(color>>8), static_cast<unsigned char>(color)};
                if (ok) ok = std::fwrite(pixel, 1, 3, file) == 3;
            }
            if (std::fclose(file) || !ok) {
                std::remove(path.c_str());
                throw std::runtime_error("capture write failed");
            }
            std::printf("{\"event\":\"capture\",\"schema\":\"gbb-elapsed-capture-v1\","
                        "\"qualification\":false,\"anchor\":\"elapsed_8mhz_after_boot_unmap_pc_0100\","
                        "\"target_8mhz\":%llu,\"clocks_8mhz\":%llu,\"completed_frames\":%u,"
                        "\"pc\":%u,\"ly\":%u,\"dot\":%u,\"lcdc\":%u}\n",
                        static_cast<unsigned long long>(target),
                        static_cast<unsigned long long>(e.bus().debug_normalized_clocks()-origin),
                        frames, e.cpu().registers().pc, e.bus().read8(0xff44),
                        e.bus().debug_ppu_dot(), e.bus().read8(0xff40));
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "elapsed capture failed: %s\n", error.what());
        return 3;
    }
}
