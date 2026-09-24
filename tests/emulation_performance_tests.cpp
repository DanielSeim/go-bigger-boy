#include "gbb/core_runtime.hpp"
#include "gameboy/cartridge.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/joypad.hpp"
#include "frame_rate_metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr unsigned cycles_per_frame = 70224;
constexpr unsigned measured_frames = 600;

int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> load_test_rom() {
    std::vector<std::uint8_t> rom(0x8000, 0);
    const std::string title = "GBB PERF LOAD";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);

    // A small deterministic workload that continuously exercises CPU, WRAM,
    // timer, APU, and PPU paths without depending on a proprietary ROM.
    constexpr std::uint8_t program[] = {
        0x3E, 0x01,             // LD A,$01
        0xEA, 0x00, 0xC0,       // LD ($C000),A
        0xEE, 0x01,             // XOR $01
        0xC3, 0x02, 0x01,       // JP $0102
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x100);
    rom[0x147] = 0x00; // ROM only
    rom[0x148] = 0x00; // 32 KiB
    rom[0x149] = 0x00; // no external RAM
    return rom;
}

double minimum_fps() {
    const auto* value = std::getenv("GBB_PERF_MIN_FPS");
    if (value == nullptr || *value == '\0') return 30.0;
    char* end = nullptr;
    const auto parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || parsed < 0.0) return 30.0;
    return parsed;
}

} // namespace

int main() {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    gameboy::Emulator emulator{gameboy::Cartridge{load_test_rom()}};
    gbb::sdl::FrameRateMetrics fps_metrics{50ms};
    std::uint64_t total_cycles = 0;
    std::uint64_t frame_checksum = 0;
    std::uint64_t audio_samples = 0;
    unsigned completed_frames = 0;
    unsigned samples = 0;
    const auto started = Clock::now();

    for (unsigned frame = 0; frame < measured_frames; ++frame) {
        emulator.set_button(gameboy::Button::a, (frame & 1U) != 0U);
        const auto advance = gbb::advance_to_frame(emulator, cycles_per_frame * 2U);
        check(advance.frame_ready,
              "the synthetic workload reaches every requested video frame");
        check(advance.cycles >= cycles_per_frame / 2U &&
                  advance.cycles <= cycles_per_frame * 2U,
              "each frame completes within a bounded emulation budget");
        if (!advance.frame_ready) break;

        total_cycles += advance.cycles;
        const auto& framebuffer = emulator.framebuffer();
        for (std::size_t index = 0; index < framebuffer.size(); index += 97) {
            frame_checksum = (frame_checksum * 131U) + framebuffer[index];
        }
        audio_samples += emulator.take_audio_samples().size() / 2U;
        emulator.consume_frame();
        ++completed_frames;
        if (fps_metrics.observe(Clock::now()).has_value()) ++samples;
    }

    const auto elapsed = std::chrono::duration<double>(Clock::now() - started)
                             .count();
    const auto measured_fps = elapsed <= 0.0
                                  ? std::numeric_limits<double>::infinity()
                                  : static_cast<double>(completed_frames) / elapsed;

    check(completed_frames == measured_frames,
          "the load test completed its full frame budget");
    check(total_cycles > static_cast<std::uint64_t>(measured_frames) *
                              cycles_per_frame / 2U,
          "the load test executed substantial emulation work");
    check(frame_checksum != 0, "the load test consumed rendered framebuffer data");
    check(audio_samples > 0, "the load test consumed generated audio samples");
    check(samples > 0, "FPS metrics published at least one load-test sample");
    check(measured_fps >= minimum_fps(),
          "the measured core FPS is above the configured performance floor");

    std::cout << std::fixed << std::setprecision(2)
              << "performance_metric name=core_emulated_fps value="
              << measured_fps << " frames=" << completed_frames
              << " elapsed_ms=" << elapsed * 1000.0 << '\n'
              << "performance_metric name=emulated_cycles value="
              << total_cycles << '\n'
              << "performance_metric name=audio_samples value="
              << audio_samples << '\n'
              << "performance_metric name=fps_windows value=" << samples << '\n'
              << "performance_metric name=frame_checksum value="
              << frame_checksum << '\n';
    return failures == 0 ? 0 : 1;
}
