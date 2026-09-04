#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
std::vector<std::uint8_t> test_rom() {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "APU TEST";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    return rom;
}
std::vector<std::uint8_t> cgb_test_rom() {
    auto rom = test_rom();
    rom[0x143] = 0x80;
    return rom;
}

void test_apu_power_registers_and_wave_ram() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot();
    check((bus.read8(0xFF26) & 0xF0) == 0xF0,
          "post-boot APU state has master power enabled");
    check(bus.read8(0xFF24) == 0x77 && bus.read8(0xFF25) == 0xF3,
          "post-boot APU mixer registers have their DMG values");
    bus.write8(0xFF27, 0);
    bus.write8(0xFF2F, 0);
    check(bus.read8(0xFF27) == 0xFF && bus.read8(0xFF2F) == 0xFF,
          "unused APU register space ignores writes and reads as FF");

    bus.write8(0xFF30, 0xA5);
    bus.write8(0xFF26, 0);
    check(bus.read8(0xFF26) == 0x70,
          "clearing NR52 powers down the APU and its channels");
    bus.write8(0xFF17, 0xF0);
    check(bus.read8(0xFF17) == 0,
          "powered-down APU registers ignore ordinary writes");
    check(bus.read8(0xFF30) == 0xA5,
          "wave RAM remains accessible while the APU is powered down");
    bus.write8(0xFF16, 0xBF); // DMG permits length writes while powered down.
    bus.write8(0xFF26, 0x80);
    check((bus.read8(0xFF26) & 0x80) != 0,
          "setting NR52 restores APU master power");
    bus.write8(0xFF17, 0xF0);
    bus.write8(0xFF19, 0xC0);
    bus.tick(4096);
    bus.write8(0xFF04, 0);
    check((bus.read8(0xFF26) & 0x02) == 0,
          "DMG length-counter writes survive an APU power cycle");

    gameboy::MemoryBus cgb{gameboy::Cartridge{cgb_test_rom()}};
    cgb.initialize_post_boot(gameboy::HardwareModel::cgb);
    cgb.write8(0xFF26, 0);
    cgb.write8(0xFF16, 0xBF); // CGB ignores length writes while powered off.
    cgb.write8(0xFF26, 0x80);
    cgb.write8(0xFF17, 0xF0);
    cgb.write8(0xFF19, 0xC0);
    cgb.tick(4096);
    cgb.write8(0xFF04, 0);
    check((cgb.read8(0xFF26) & 0x02) != 0,
          "CGB power-off clears lengths and ignores powered-down length writes");
    cgb.write8(0xFF11, 0x80);
    cgb.write8(0xFF12, 0xF0);
    cgb.write8(0xFF13, 0xFC);
    cgb.write8(0xFF14, 0x87);
    auto saw_pcm_pulse = false;
    for (unsigned cycle = 0; cycle < 128 && !saw_pcm_pulse; ++cycle) {
        saw_pcm_pulse = (cgb.read8(0xFF76) & 0x0F) == 0x0F;
        cgb.tick(1);
    }
    check(saw_pcm_pulse && bus.read8(0xFF76) == 0xFF,
          "CGB PCM12 exposes live channel output and remains unmapped on DMG");

    const auto pcm_after_trigger = [](const unsigned idle_cycles) {
        gameboy::MemoryBus probe{gameboy::Cartridge{cgb_test_rom()}};
        probe.initialize_post_boot(gameboy::HardwareModel::cgb);
        probe.write8(0xFF11, 0x40); // 25% duty, initially high.
        probe.write8(0xFF12, 0x80); // DAC enabled, volume 8.
        probe.write8(0xFF13, 0xFF);
        probe.write8(0xFF14, 0x07); // Leave channel disabled.
        probe.tick(idle_cycles);
        probe.write8(0xFF14, 0x87); // Trigger after the idle interval.
        return probe.read8(0xFF76) & 0x0F;
    };
    check(pcm_after_trigger(0) == pcm_after_trigger(256),
          "disabled pulse timers do not advance the duty phase");
}

void test_active_wave_ram_timing() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot();
    for (unsigned index = 0; index < 16; ++index) {
        bus.write8(static_cast<std::uint16_t>(0xFF30 + index),
                   static_cast<std::uint8_t>(0x40 + index));
    }
    bus.write8(0xFF1A, 0x80);
    bus.write8(0xFF1D, 0xFC); // Frequency 2044: one sample every 8 clocks.
    bus.write8(0xFF1E, 0x87);

    check(bus.read8(0xFF30) == 0xFF,
          "active wave RAM is blocked between channel 3 fetches");
    bus.tick(14); // Trigger adds a six-clock startup delay.
    check(bus.read8(0xFF3F) == 0x40,
          "active wave RAM addresses expose the byte channel 3 just fetched");
    bus.write8(0xFF3F, 0xA5);
    bus.tick(2);
    bus.write8(0xFF30, 0x99);
    check(bus.read8(0xFF30) == 0xFF,
          "the channel 3 wave RAM access window lasts two clocks");
    bus.write8(0xFF1A, 0);
    check(bus.read8(0xFF30) == 0xA5,
          "active wave writes target the fetched byte and blocked writes are ignored");

    gameboy::MemoryBus corruption{gameboy::Cartridge{test_rom()}};
    corruption.initialize_post_boot();
    for (unsigned index = 0; index < 16; ++index) {
        corruption.write8(static_cast<std::uint16_t>(0xFF30 + index),
                          static_cast<std::uint8_t>(index));
    }
    corruption.write8(0xFF1A, 0x80);
    corruption.write8(0xFF1D, 0xFC);
    corruption.write8(0xFF1E, 0x87);
    corruption.tick(68); // Channel 3 is about to fetch wave byte 4.
    corruption.write8(0xFF1E, 0x87);
    corruption.write8(0xFF1A, 0);
    check(corruption.read8(0xFF30) == 4 &&
              corruption.read8(0xFF31) == 5 &&
              corruption.read8(0xFF32) == 6 &&
              corruption.read8(0xFF33) == 7,
          "retriggering channel 3 during a fetch reproduces DMG wave corruption");

    gameboy::MemoryBus cgb{gameboy::Cartridge{cgb_test_rom()}};
    cgb.initialize_post_boot(gameboy::HardwareModel::cgb);
    for (unsigned index = 0; index < 16; ++index) {
        cgb.write8(static_cast<std::uint16_t>(0xFF30 + index),
                   static_cast<std::uint8_t>(index));
    }
    cgb.write8(0xFF1A, 0x80);
    cgb.write8(0xFF1D, 0xFC);
    cgb.write8(0xFF1E, 0x87);
    check(cgb.read8(0xFF3F) == 0,
          "active CGB wave RAM reads are redirected to the current byte");
    cgb.write8(0xFF3F, 0xA5);
    cgb.tick(68);
    cgb.write8(0xFF1E, 0x87);
    cgb.write8(0xFF1A, 0);
    check(cgb.read8(0xFF30) == 0xA5 && cgb.read8(0xFF31) == 1 &&
              cgb.read8(0xFF32) == 2 && cgb.read8(0xFF33) == 3,
          "CGB wave writes redirect while active and retriggering does not corrupt RAM");
}

void test_apu_high_pass_filter() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot();
    bus.write8(0xFF24, 0x77);
    bus.write8(0xFF25, 0x11); // Inactive channel 1 DAC routed both ways.
    bus.tick(41943);
    const auto filtered = bus.take_audio_samples();
    const auto magnitude = [](const std::int16_t sample) {
        return sample < 0 ? -static_cast<int>(sample) : static_cast<int>(sample);
    };
    check(filtered.size() > 4 &&
              magnitude(filtered.front()) >
                  magnitude(filtered[filtered.size() - 2]),
          "the DMG high-pass filter removes an inactive DAC's DC bias");

    bus.write8(0xFF12, 0); // Disconnect the final active DAC.
    bus.tick(4096);
    const auto disconnected = bus.take_audio_samples();
    check(std::all_of(disconnected.begin(), disconnected.end(),
                      [](const std::int16_t sample) { return sample == 0; }),
          "disconnecting every DAC forces the mixed output to silence");

    gameboy::MemoryBus cgb{gameboy::Cartridge{cgb_test_rom()}};
    cgb.initialize_post_boot(gameboy::HardwareModel::cgb);
    cgb.write8(0xFF24, 0x77);
    cgb.write8(0xFF25, 0x11);
    cgb.tick(41943);
    const auto cgb_filtered = cgb.take_audio_samples();
    check(!cgb_filtered.empty() &&
              magnitude(cgb_filtered[cgb_filtered.size() - 2]) <
                  magnitude(filtered[filtered.size() - 2]),
          "CGB output uses its faster hardware high-pass response");
}

void test_apu_pulse2_samples_and_length() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot();
    bus.write8(0xFF24, 0x77); // Full left and right master volume.
    bus.write8(0xFF25, 0x22); // Route channel 2 to both outputs.
    bus.write8(0xFF16, 0x80); // 50% duty.
    bus.write8(0xFF17, 0xF0); // Initial volume 15, envelope disabled.
    bus.write8(0xFF18, 0x00);
    bus.write8(0xFF19, 0x87); // Trigger at frequency 1792.
    check((bus.read8(0xFF26) & 0x02) != 0,
          "triggering pulse channel 2 marks it active in NR52");

    bus.tick(41943); // Approximately 10 ms at the DMG master clock.
    const auto samples = bus.take_audio_samples();
    check(samples.size() >= 950 && samples.size() <= 970 &&
              samples.size() % 2 == 0,
          "the APU resamples master-clock cycles into 48 kHz stereo frames");
    check(std::any_of(samples.begin(), samples.end(),
                      [](const std::int16_t sample) { return sample != 0; }),
          "an active pulse channel produces audible non-zero PCM samples");
    auto stereo_matches = true;
    for (std::size_t index = 0; index + 1 < samples.size(); index += 2) {
        stereo_matches = stereo_matches && samples[index] == samples[index + 1];
    }
    check(stereo_matches,
          "routing pulse channel 2 to both terminals produces matching stereo");

    gameboy::MemoryBus length_bus{gameboy::Cartridge{test_rom()}};
    length_bus.initialize_post_boot();
    length_bus.write8(0xFF16, 0xBF); // 50% duty and a one-tick length.
    length_bus.write8(0xFF17, 0xF0);
    length_bus.write8(0xFF19, 0xC0); // Trigger with length enabled.
    length_bus.tick(4096);           // Raise the internal DIV-APU bit.
    length_bus.write8(0xFF04, 0);    // Its falling edge clocks length.
    check((length_bus.read8(0xFF26) & 0x02) == 0,
          "resetting DIV on its APU edge clocks and expires channel length");

    gameboy::MemoryBus extra_clock_bus{gameboy::Cartridge{test_rom()}};
    extra_clock_bus.initialize_post_boot();
    extra_clock_bus.write8(0xFF16, 0xBF);
    extra_clock_bus.write8(0xFF17, 0xF0);
    extra_clock_bus.write8(0xFF19, 0x80); // Trigger with length disabled.
    extra_clock_bus.tick(8192);           // Next sequencer step skips length.
    extra_clock_bus.write8(0xFF19, 0x40); // Enabling length adds a clock.
    check((extra_clock_bus.read8(0xFF26) & 0x02) == 0,
          "enabling length before a non-length step performs the DMG extra clock");

    gameboy::MemoryBus apu_start_phase{gameboy::Cartridge{cgb_test_rom()}};
    apu_start_phase.initialize_post_boot(gameboy::HardwareModel::cgb);
    apu_start_phase.write8(0xFF26, 0x00); // Power the APU off.
    apu_start_phase.write8(0xFF04, 0x00);
    apu_start_phase.tick(4096); // Leave DIV/APU high before re-enabling.
    apu_start_phase.write8(0xFF26, 0x80);
    apu_start_phase.write8(0xFF16, 0xBF); // One-tick channel-2 length.
    apu_start_phase.write8(0xFF17, 0xF0);
    apu_start_phase.write8(0xFF19, 0xC0);
    apu_start_phase.tick(4096); // The first falling edge is skipped.
    check((apu_start_phase.read8(0xFF26) & 0x02) != 0,
          "enabling the APU on a high DIV/APU phase skips its first edge");
    apu_start_phase.tick(8192); // The next falling edge clocks the length.
    check((apu_start_phase.read8(0xFF26) & 0x02) == 0,
          "the skipped APU edge does not shift later frame-sequencer clocks");
}

void test_apu_pulse1_sweep_wave_and_noise() {
    gameboy::MemoryBus pulse_bus{gameboy::Cartridge{test_rom()}};
    pulse_bus.initialize_post_boot();
    pulse_bus.write8(0xFF24, 0x77);
    pulse_bus.write8(0xFF25, 0x01); // Channel 1 to right output only.
    pulse_bus.write8(0xFF10, 0x11); // Sweep pace 1, add, shift 1.
    pulse_bus.write8(0xFF11, 0x80);
    pulse_bus.write8(0xFF12, 0xF0);
    pulse_bus.write8(0xFF13, 0xE8); // Frequency 1000.
    pulse_bus.write8(0xFF14, 0x83);
    check((pulse_bus.read8(0xFF26) & 0x01) != 0,
          "triggering pulse channel 1 marks it active in NR52");
    pulse_bus.tick(4096);
    const auto pulse_samples = pulse_bus.take_audio_samples();
    auto right_only_audio = false;
    for (std::size_t index = 0; index + 1 < pulse_samples.size(); index += 2) {
        right_only_audio = right_only_audio ||
                           (pulse_samples[index] == 0 &&
                            pulse_samples[index + 1] != 0);
    }
    check(right_only_audio,
          "NR51 can route pulse channel 1 exclusively to the right terminal");
    pulse_bus.tick(3 * 8192);
    check((pulse_bus.read8(0xFF26) & 0x01) == 0,
          "pulse channel 1 sweep disables the channel on frequency overflow");

    gameboy::MemoryBus overflow_bus{gameboy::Cartridge{test_rom()}};
    overflow_bus.initialize_post_boot();
    overflow_bus.write8(0xFF10, 0x01); // Pace zero, add, shift one.
    overflow_bus.write8(0xFF11, 0x80);
    overflow_bus.write8(0xFF12, 0xF0);
    overflow_bus.write8(0xFF13, 0xDC); // Frequency 1500.
    overflow_bus.write8(0xFF14, 0x85);
    check((overflow_bus.read8(0xFF26) & 0x01) == 0,
          "pulse channel 1 checks sweep overflow even when pace is zero");

    gameboy::MemoryBus wave_bus{gameboy::Cartridge{test_rom()}};
    wave_bus.initialize_post_boot();
    wave_bus.write8(0xFF24, 0x77);
    wave_bus.write8(0xFF25, 0x44); // Channel 3 to both terminals.
    for (unsigned index = 0; index < 16; ++index) {
        wave_bus.write8(static_cast<std::uint16_t>(0xFF30 + index),
                        index % 2 == 0 ? 0xF0 : 0x1E);
    }
    wave_bus.write8(0xFF1A, 0x80);
    wave_bus.write8(0xFF1C, 0x20); // Full output level.
    wave_bus.write8(0xFF1D, 0x00);
    wave_bus.write8(0xFF1E, 0x87);
    wave_bus.tick(4096);
    const auto wave_samples = wave_bus.take_audio_samples();
    check((wave_bus.read8(0xFF26) & 0x04) != 0 &&
              std::any_of(wave_samples.begin(), wave_samples.end(),
                          [](const std::int16_t sample) { return sample != 0; }),
          "wave channel 3 plays packed four-bit samples from wave RAM");

    gameboy::MemoryBus noise_bus{gameboy::Cartridge{test_rom()}};
    noise_bus.initialize_post_boot();
    noise_bus.write8(0xFF24, 0x77);
    noise_bus.write8(0xFF25, 0x88); // Channel 4 to both terminals.
    noise_bus.write8(0xFF21, 0xF0);
    noise_bus.write8(0xFF22, 0x08); // Fast 7-bit LFSR mode.
    noise_bus.write8(0xFF23, 0x80);
    noise_bus.tick(4096);
    const auto noise_samples = noise_bus.take_audio_samples();
    check((noise_bus.read8(0xFF26) & 0x08) != 0 &&
              std::any_of(noise_samples.begin(), noise_samples.end(),
                          [](const std::int16_t sample) { return sample != 0; }),
          "noise channel 4 produces PCM through its short-mode LFSR");

    noise_bus.write8(0xFF21, 0);
    check((noise_bus.read8(0xFF26) & 0x08) == 0,
          "disabling a channel DAC immediately clears its NR52 status bit");
}


} // namespace

int main() {
    test_apu_power_registers_and_wave_ram();
    test_active_wave_ram_timing();
    test_apu_pulse1_sweep_wave_and_noise();
    test_apu_pulse2_samples_and_length();
    test_apu_high_pass_filter();
    return failures == 0 ? 0 : 1;
}
