#include "gbb/core_runtime.hpp"
#include "gameboy/cartridge.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/hardware_model.hpp"
#include "gameboy/ppu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

constexpr unsigned cycles_per_frame = 70'224;
constexpr unsigned warmup_frames = 12;
constexpr unsigned measured_frames = 180;

std::vector<std::uint8_t> test_rom() {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "SGB PERF";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    constexpr std::array<std::uint8_t, 10> program{
        0x3E, 0x01, 0xEA, 0x00, 0xC0, 0xEE, 0x01, 0xC3, 0x02, 0x01};
    std::copy(program.begin(), program.end(), rom.begin() + 0x100);
    rom[0x146] = 0x03;
    return rom;
}

void seed_video(gameboy::Emulator& emulator) {
    for (unsigned tile = 0; tile < 64; ++tile) {
        for (unsigned row = 0; row < 8; ++row) {
            const auto offset = static_cast<std::uint16_t>(tile * 16 + row * 2);
            emulator.bus().debug_write_vram(
                0, offset, static_cast<std::uint8_t>((tile + row) & 1U ? 0xAA : 0x55));
            emulator.bus().debug_write_vram(
                0, offset + 1, static_cast<std::uint8_t>(row & 1U ? 0xF0 : 0x0F));
        }
    }
    for (unsigned y = 0; y < 32; ++y) {
        for (unsigned x = 0; x < 32; ++x) {
            emulator.bus().debug_write_vram(
                0, static_cast<std::uint16_t>(0x1800 + y * 32 + x),
                static_cast<std::uint8_t>((x + y * 7) % 64));
        }
    }
    for (unsigned sprite = 0; sprite < 40; ++sprite) {
        const auto base = static_cast<std::uint8_t>(sprite * 4);
        emulator.bus().debug_write_oam(
            base, static_cast<std::uint8_t>(16 + (sprite * 17) % 112));
        emulator.bus().debug_write_oam(
            static_cast<std::uint8_t>(base + 1),
            static_cast<std::uint8_t>(8 + (sprite * 29) % 144));
        emulator.bus().debug_write_oam(
            static_cast<std::uint8_t>(base + 2),
            static_cast<std::uint8_t>(sprite % 64));
        emulator.bus().debug_write_oam(
            static_cast<std::uint8_t>(base + 3), 0);
    }
    emulator.bus().write8(0xFF40, 0x93); // BG and OBJ enabled
}

void send_sgb_packet(gameboy::Emulator& emulator, const std::uint8_t command) {
    std::array<std::uint8_t, 16> packet{};
    packet[0] = static_cast<std::uint8_t>((command << 3) | 1U);
    const auto write = [&](const std::uint8_t value) {
        emulator.bus().write8(0xFF00, value);
    };
    write(0x30);
    write(0x00);
    for (unsigned bit = 0; bit < 128; ++bit) {
        write(0x30);
        write((packet[bit / 8] & (1U << (bit & 7U))) != 0 ? 0x10 : 0x20);
    }
    write(0x30);
    write(0x20);
}

double minimum_fps() {
    const auto* value = std::getenv("GBB_SGB_PERF_MIN_FPS");
    if (value == nullptr || *value == '\0') return 60.0;
    char* end = nullptr;
    const auto parsed = std::strtod(value, &end);
    return end != value && *end == '\0' && parsed >= 0.0 ? parsed : 60.0;
}

struct Result {
    const char* mode;
    double core_fps;
    double frame_fps;
    double compose_us;
    double compose_max_us;
    std::uint64_t checksum;
    unsigned completed;
};

Result run_case(const char* mode, const gameboy::HardwareModel model,
                const bool transferred_border) {
    using Clock = std::chrono::steady_clock;
    gameboy::Emulator emulator{gameboy::Cartridge{test_rom()}, model};
    seed_video(emulator);
    for (unsigned frame = 0; frame < warmup_frames; ++frame) {
        const auto advance = gbb::advance_to_frame(emulator, cycles_per_frame * 2);
        if (!advance.frame_ready) return {mode, 0, 0, 0, 0, 0, frame};
        emulator.consume_frame();
    }
    if (transferred_border) {
        send_sgb_packet(emulator, 0x13); // CHR_TRN
        for (unsigned frame = 0; frame < 6; ++frame) {
            if (!gbb::advance_to_frame(emulator, cycles_per_frame * 2).frame_ready)
                return {mode, 0, 0, 0, 0, 0, 0};
            emulator.consume_frame();
        }
        send_sgb_packet(emulator, 0x14); // PCT_TRN
        for (unsigned frame = 0; frame < 6; ++frame) {
            if (!gbb::advance_to_frame(emulator, cycles_per_frame * 2).frame_ready)
                return {mode, 0, 0, 0, 0, 0, 0};
            emulator.consume_frame();
        }
        if (emulator.bus().debug_sgb_diagnostics().commands_applied != 2)
            return {mode, 0, 0, 0, 0, 0, 0};
    }

    std::chrono::nanoseconds core_time{};
    std::chrono::nanoseconds frame_time{};
    std::chrono::nanoseconds compose_time{};
    std::chrono::nanoseconds compose_max{};
    std::uint64_t checksum = 0;
    unsigned completed = 0;
    for (unsigned frame = 0; frame < measured_frames; ++frame) {
        const auto core_started = Clock::now();
        const auto advance = gbb::advance_to_frame(emulator, cycles_per_frame * 2);
        const auto core_finished = Clock::now();
        if (!advance.frame_ready) break;
        core_time += core_finished - core_started;

        const auto compose_started = Clock::now();
        if (model == gameboy::HardwareModel::sgb) {
            const auto& pixels = emulator.sgb_framebuffer();
            for (std::size_t i = 0; i < pixels.size(); i += 127)
                checksum = checksum * 131 + pixels[i];
        } else {
            const auto& pixels = emulator.framebuffer();
            for (std::size_t i = 0; i < pixels.size(); i += 127)
                checksum = checksum * 131 + pixels[i];
        }
        const auto compose_duration = Clock::now() - compose_started;
        compose_time += compose_duration;
        compose_max = std::max(compose_max,
                               std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   compose_duration));
        static_cast<void>(emulator.take_audio_samples());
        emulator.consume_frame();
        frame_time += Clock::now() - core_started;
        ++completed;
    }
    const auto core_seconds = std::chrono::duration<double>(core_time).count();
    const auto frame_seconds = std::chrono::duration<double>(frame_time).count();
    return {mode,
            core_seconds > 0 ? completed / core_seconds : 0,
            frame_seconds > 0 ? completed / frame_seconds : 0,
            completed > 0
                ? std::chrono::duration<double, std::micro>(compose_time).count() /
                      completed
                : 0,
            std::chrono::duration<double, std::micro>(compose_max).count(),
            checksum, completed};
}

} // namespace

int main() {
    const std::array results{
        run_case("dmg", gameboy::HardwareModel::dmg, false),
        run_case("sgb_fallback", gameboy::HardwareModel::sgb, false),
        run_case("sgb_transferred", gameboy::HardwareModel::sgb, true),
    };
    int failures = 0;
    for (const auto& result : results) {
        std::cout << std::fixed << std::setprecision(2)
                  << "sgb_performance_metric mode=" << result.mode
                  << " core_fps=" << result.core_fps
                  << " frame_fps=" << result.frame_fps
                  << " compose_avg_us=" << result.compose_us
                  << " compose_max_us=" << result.compose_max_us
                  << " frames=" << result.completed
                  << " checksum=" << result.checksum << '\n';
        if (result.completed != measured_frames || result.checksum == 0 ||
            result.frame_fps < minimum_fps()) {
            std::cerr << "FAIL: SGB performance case " << result.mode
                      << " did not meet the emulation floor\n";
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
