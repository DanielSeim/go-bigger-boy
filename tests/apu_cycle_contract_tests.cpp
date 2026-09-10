#include "gameboy/memory_bus.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> test_rom() {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "APU CONTRACT";
    for (std::size_t index = 0; index < title.size(); ++index) {
        rom[0x134 + index] = static_cast<std::uint8_t>(title[index]);
    }
    return rom;
}

void test_cycle_integrated_resampling() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot();
    bus.write8(0xFF24, 0x77); // Full left and right master volume.
    bus.write8(0xFF25, 0x11); // Route channel 1 to both terminals.

    // Let one active channel cycle contribute for a single master-clock tick,
    // then remove its routing before the 48 kHz sample boundary. A sampler
    // that only observes the final cycle would output silence; integration
    // must preserve the short transition as a small non-zero sample.
    bus.tick(1);
    bus.write8(0xFF25, 0x00);
    bus.tick(87);
    const auto samples = bus.take_audio_samples();
    check(samples.size() == 2 && samples[0] != 0 && samples[1] != 0,
          "audio resampling preserves transitions shorter than one output sample");
    check(samples[0] == samples[1],
          "cycle-integrated mixer keeps both routed terminals phase-aligned");
}

void test_audio_generation_can_be_disabled() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot();
    bus.write8(0xFF11, 0x80); // 50% duty.
    bus.write8(0xFF12, 0xF3); // DAC on, audible envelope.
    bus.write8(0xFF13, 0x40);
    bus.write8(0xFF14, 0x87); // Trigger channel 1.
    bus.set_audio_enabled(false);
    bus.tick(4096);
    check(bus.take_audio_samples().empty(),
          "disabled audio generation does not emit samples");
    check(bus.audio_enabled() == false,
          "audio generation reports the disabled state");

    bus.set_audio_enabled(true);
    bus.tick(4096);
    check(!bus.take_audio_samples().empty(),
          "re-enabling audio resumes sample generation");
}

} // namespace

int main() {
    test_cycle_integrated_resampling();
    test_audio_generation_can_be_disabled();
    return failures == 0 ? 0 : 1;
}
