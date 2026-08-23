#include "gameboy/cpu.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint16_t program_address = 0x0100;
int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> test_rom(
    const std::vector<std::uint8_t>& program = {}) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    const std::string title = "CORE TEST";
    for (std::size_t i = 0; i < title.size(); ++i) {
        rom[0x134 + i] = static_cast<std::uint8_t>(title[i]);
    }
    for (std::size_t i = 0; i < program.size(); ++i) {
        rom[program_address + i] = program[i];
    }
    return rom;
}

std::vector<std::uint8_t> banked_rom(const unsigned banks,
                                     const std::uint8_t type,
                                     const std::uint8_t rom_size_code,
                                     const std::uint8_t ram_size_code) {
    std::vector<std::uint8_t> rom(static_cast<std::size_t>(banks) * 0x4000);
    for (unsigned bank = 0; bank < banks; ++bank) {
        std::fill(rom.begin() + static_cast<std::size_t>(bank) * 0x4000,
                  rom.begin() + static_cast<std::size_t>(bank + 1) * 0x4000,
                  static_cast<std::uint8_t>(bank));
        rom[static_cast<std::size_t>(bank) * 0x4000 + 1] =
            static_cast<std::uint8_t>(bank >> 8);
    }
    const std::string title = "MBC1 TEST";
    for (std::size_t index = 0; index < title.size(); ++index) {
        rom[0x134 + index] = static_cast<std::uint8_t>(title[index]);
    }
    rom[0x147] = type;
    rom[0x148] = rom_size_code;
    rom[0x149] = ram_size_code;
    return rom;
}

gameboy::CpuRegisters initial_registers() {
    return {
        0x77, 0xB0, 0x11, 0x22, 0x33, 0x44, 0xC0, 0x00, 0xFFFE,
        program_address,
    };
}

std::uint8_t register_value(const gameboy::CpuRegisters& registers,
                            const unsigned index) {
    switch (index) {
    case 0: return registers.b;
    case 1: return registers.c;
    case 2: return registers.d;
    case 3: return registers.e;
    case 4: return registers.h;
    case 5: return registers.l;
    default: return registers.a;
    }
}

void test_cartridge_header() {
    gameboy::Cartridge cartridge{test_rom()};
    check(cartridge.title() == "CORE TEST", "cartridge title is parsed");
    check(cartridge.rom_size() == 0x8000, "ROM size is reported");
}

void test_cartridge_file_loading() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("gameboy-loader-test-" + std::to_string(unique) + ".gb");
    const auto rom = test_rom();
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(rom.data()),
                     static_cast<std::streamsize>(rom.size()));
    }
    const auto cartridge = gameboy::Cartridge::from_file(path);
    std::filesystem::remove(path);
    check(cartridge.rom_size() == rom.size() && cartridge.title() == "CORE TEST",
          "cartridge loader accepts a successfully read ROM file");
}

void test_mbc1_rom_banking() {
    gameboy::Cartridge cartridge{banked_rom(128, 0x01, 0x06, 0x00)};
    check(cartridge.read(0x4000) == 1,
          "MBC1 powers up with ROM bank 1 selected");
    cartridge.write(0x2000, 2);
    check(cartridge.read(0x4000) == 2,
          "MBC1 low register selects the switchable ROM bank");
    cartridge.write(0x2000, 0);
    check(cartridge.read(0x4000) == 1,
          "MBC1 remaps low bank value zero to one");
    cartridge.write(0x2000, 2);
    cartridge.write(0x4000, 1);
    check(cartridge.read(0x4000) == 34,
          "MBC1 combines upper and lower ROM bank registers");
    check(cartridge.read(0x0000) == 0,
          "MBC1 mode 0 keeps the lower ROM window at bank zero");
    cartridge.write(0x6000, 1);
    check(cartridge.read(0x0000) == 32 && cartridge.read(0x4000) == 34,
          "MBC1 mode 1 maps upper bits into both ROM windows");

    gameboy::Cartridge masked{banked_rom(16, 0x01, 0x03, 0x00)};
    masked.write(0x2000, 0x10);
    check(masked.read(0x4000) == 0,
          "MBC1 bank-zero translation occurs before physical ROM masking");
}

void test_mbc1_ram_banking() {
    gameboy::Cartridge cartridge{banked_rom(32, 0x02, 0x04, 0x03)};
    check(cartridge.ram_size() == 0x8000 && !cartridge.has_battery(),
          "MBC1+RAM allocates header-declared non-battery RAM");
    cartridge.write(0xA000, 0x11);
    check(cartridge.read(0xA000) == 0xFF,
          "disabled MBC1 RAM reads as open bus and ignores writes");
    cartridge.write(0x0000, 0x1A);
    cartridge.write(0xA000, 0x10);
    cartridge.write(0x6000, 1);
    for (std::uint8_t bank = 1; bank < 4; ++bank) {
        cartridge.write(0x4000, bank);
        cartridge.write(0xA000, static_cast<std::uint8_t>(0x10 + bank));
    }
    cartridge.write(0x4000, 0);
    check(cartridge.read(0xA000) == 0x10,
          "MBC1 advanced mode retains RAM bank zero");
    for (std::uint8_t bank = 1; bank < 4; ++bank) {
        cartridge.write(0x4000, bank);
        check(cartridge.read(0xA000) == static_cast<std::uint8_t>(0x10 + bank),
              "MBC1 advanced mode selects independent RAM banks");
    }
    cartridge.write(0x6000, 0);
    check(cartridge.read(0xA000) == 0x10,
          "MBC1 simple mode locks external RAM to bank zero");
    cartridge.write(0x0000, 0);
    check(cartridge.read(0xA000) == 0xFF,
          "disabling MBC1 RAM restores open-bus reads");

    gameboy::Cartridge large{banked_rom(64, 0x02, 0x05, 0x02)};
    large.write(0x0000, 0x0A);
    large.write(0x6000, 1);
    large.write(0x4000, 3);
    large.write(0xA000, 0x77);
    large.write(0x4000, 0);
    check(large.read(0xA000) == 0x77,
          "large-ROM MBC1 wiring keeps its 8 KiB RAM unbanked");
}

void test_battery_ram_persistence() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto rom_path = std::filesystem::temp_directory_path() /
                          ("gameboy-battery-test-" + std::to_string(unique) +
                           ".gb");
    auto save_path = rom_path;
    save_path.replace_extension(".sav");
    const auto rom = banked_rom(4, 0x03, 0x01, 0x02);
    {
        std::ofstream output(rom_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(rom.data()),
                     static_cast<std::streamsize>(rom.size()));
    }
    {
        auto cartridge = gameboy::Cartridge::from_file(rom_path);
        check(cartridge.has_battery() && cartridge.ram_size() == 0x2000,
              "battery cartridge reports its persistent RAM capacity");
        cartridge.write(0x0000, 0x0A);
        cartridge.write(0xA000, 0x5A);
        cartridge.write(0xBFFF, 0xC3);
        cartridge.flush_battery();
    }
    check(std::filesystem::exists(save_path) &&
              std::filesystem::file_size(save_path) == 0x2000,
          "battery flush writes a complete sibling .sav file");
    {
        auto cartridge = gameboy::Cartridge::from_file(rom_path);
        cartridge.write(0x0000, 0x0A);
        check(cartridge.read(0xA000) == 0x5A &&
                  cartridge.read(0xBFFF) == 0xC3,
              "battery RAM reloads into a new cartridge instance");
    }
    std::filesystem::remove(save_path);
    std::filesystem::remove(rom_path);
}

void test_mbc3_banking_and_rtc() {
    gameboy::Cartridge cartridge{banked_rom(128, 0x10, 0x06, 0x03)};
    check(cartridge.read(0x4000) == 1,
          "MBC3 powers up with ROM bank 1 selected");
    cartridge.write(0x2000, 0);
    check(cartridge.read(0x4000) == 1,
          "MBC3 remaps switchable ROM bank zero to bank one");
    cartridge.write(0x2000, 0x20);
    check(cartridge.read(0x4000) == 0x20 && cartridge.read(0x0000) == 0,
          "MBC3 selects a seven-bit ROM bank while keeping bank zero fixed");

    cartridge.write(0x0000, 0x0A);
    for (std::uint8_t bank = 0; bank < 4; ++bank) {
        cartridge.write(0x4000, bank);
        cartridge.write(0xA000, static_cast<std::uint8_t>(0x30 + bank));
    }
    for (std::uint8_t bank = 0; bank < 4; ++bank) {
        cartridge.write(0x4000, bank);
        check(cartridge.read(0xA000) == static_cast<std::uint8_t>(0x30 + bank),
              "MBC3 selects independent external RAM banks");
    }

    cartridge.write(0x4000, 0x0C);
    cartridge.write(0xA000, 0xC1); // Halt, carry, and day bit 8.
    cartridge.write(0x4000, 0x08);
    cartridge.write(0xA000, 42);
    cartridge.write(0x4000, 0x09);
    cartridge.write(0xA000, 37);
    cartridge.write(0x4000, 0x0A);
    cartridge.write(0xA000, 19);
    cartridge.write(0x4000, 0x0B);
    cartridge.write(0xA000, 0xA5);
    cartridge.write(0x6000, 0);
    cartridge.write(0x6000, 1);
    cartridge.write(0x4000, 0x08);
    check(cartridge.read(0xA000) == 42,
          "MBC3 latches and reads the RTC seconds register");
    cartridge.write(0x4000, 0x0A);
    check(cartridge.read(0xA000) == 19,
          "MBC3 latches and reads the RTC hours register");
    cartridge.write(0x4000, 0x0B);
    check(cartridge.read(0xA000) == 0xA5,
          "MBC3 latches and reads the RTC day counter");
    cartridge.write(0x4000, 0x0C);
    check(cartridge.read(0xA000) == 0xC1,
          "MBC3 preserves RTC day-high, halt, and carry flags");

    cartridge.write(0x4000, 0x08);
    cartridge.write(0xA000, 7);
    check(cartridge.read(0xA000) == 42,
          "MBC3 RTC reads remain stable until the next latch transition");
    cartridge.write(0x6000, 0);
    cartridge.write(0x6000, 1);
    check(cartridge.read(0xA000) == 7,
          "MBC3 refreshes its RTC snapshot on a zero-to-one latch transition");
    cartridge.write(0x0000, 0);
    check(cartridge.read(0xA000) == 0xFF,
          "disabled MBC3 RAM and RTC registers read as open bus");
}

void test_mbc3_rtc_persistence() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto rom_path = std::filesystem::temp_directory_path() /
                          ("gameboy-rtc-test-" + std::to_string(unique) + ".gb");
    auto rtc_path = rom_path;
    rtc_path.replace_extension(".rtc");
    auto save_path = rom_path;
    save_path.replace_extension(".sav");
    const auto rom = banked_rom(2, 0x0F, 0x00, 0x00);
    {
        std::ofstream output(rom_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(rom.data()),
                     static_cast<std::streamsize>(rom.size()));
    }
    {
        auto cartridge = gameboy::Cartridge::from_file(rom_path);
        cartridge.write(0x0000, 0x0A);
        cartridge.write(0x4000, 0x0C);
        cartridge.write(0xA000, 0x40); // Halt for deterministic persistence.
        cartridge.write(0x4000, 0x08);
        cartridge.write(0xA000, 51);
        cartridge.flush_battery();
    }
    check(std::filesystem::exists(rtc_path) &&
              std::filesystem::file_size(rtc_path) == 21 &&
              !std::filesystem::exists(save_path),
          "timer-only MBC3 cartridges persist a sibling RTC file without RAM");
    {
        auto cartridge = gameboy::Cartridge::from_file(rom_path);
        cartridge.write(0x0000, 0x0A);
        cartridge.write(0x6000, 0);
        cartridge.write(0x6000, 1);
        cartridge.write(0x4000, 0x08);
        check(cartridge.read(0xA000) == 51,
              "MBC3 reloads persisted RTC state into a new cartridge instance");
    }
    std::filesystem::remove(rtc_path);
    std::filesystem::remove(rom_path);
}

void test_mbc5_banking_and_rumble() {
    gameboy::Cartridge cartridge{banked_rom(512, 0x1B, 0x08, 0x04)};
    check(cartridge.has_battery() && cartridge.read(0x4000) == 1,
          "MBC5 battery cartridges load with ROM bank one selected");
    cartridge.write(0x2000, 0);
    check(cartridge.read(0x4000) == 0,
          "MBC5 permits ROM bank zero in the switchable window");
    cartridge.write(0x2000, 1);
    cartridge.write(0x3000, 1);
    check(cartridge.read(0x4000) == 1 && cartridge.read(0x4001) == 1,
          "MBC5 combines its low eight and high one ROM-bank bits");
    check(cartridge.read(0x0001) == 0,
          "MBC5 keeps the lower ROM window fixed at bank zero");

    cartridge.write(0x0000, 0x0A);
    cartridge.write(0x4000, 0);
    cartridge.write(0xA000, 0x55);
    cartridge.write(0x4000, 15);
    cartridge.write(0xA000, 0xAA);
    cartridge.write(0x4000, 0);
    check(cartridge.read(0xA000) == 0x55,
          "MBC5 selects external RAM bank zero");
    cartridge.write(0x4000, 15);
    check(cartridge.read(0xA000) == 0xAA,
          "MBC5 uses all four RAM-bank bits on non-rumble cartridges");

    gameboy::Cartridge rumble{banked_rom(2, 0x1E, 0x00, 0x03)};
    check(rumble.has_rumble() && !rumble.rumble_active(),
          "MBC5 rumble cartridges expose an initially inactive motor");
    rumble.write(0x0000, 0x0A);
    rumble.write(0x4000, 0x0B); // Rumble on, RAM bank 3.
    rumble.write(0xA000, 0x77);
    check(rumble.rumble_active(), "MBC5 bit three activates rumble");
    rumble.write(0x4000, 3);
    check(!rumble.rumble_active() && rumble.read(0xA000) == 0x77,
          "MBC5 rumble uses only the low three bits for its RAM bank");
}

void test_memory_map() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.write8(0xC123, 0x42);
    check(bus.read8(0xC123) == 0x42, "work RAM can be read and written");
    check(bus.read8(0xE123) == 0x42, "echo RAM mirrors work RAM");

    bus.write8(0xE456, 0x99);
    check(bus.read8(0xC456) == 0x99, "echo RAM writes mirror work RAM");

    bus.write16(0xFF80, 0xBEEF);
    check(bus.read16(0xFF80) == 0xBEEF, "16-bit accesses are little endian");

    bus.write8(0xFEA0, 0x12);
    check(bus.read8(0xFEA0) == 0xFF, "unusable memory reads as FF");
}

void test_apu_power_registers_and_wave_ram() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.initialize_post_boot();
    check(bus.read8(0xFF26) == 0xF0,
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

void test_serial_transfer() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.write8(0xFF0F, 0);
    bus.write8(0xFF01, 'A');
    bus.write8(0xFF02, 0x81);
    check(bus.read8(0xFF01) == 'A' && (bus.read8(0xFF02) & 0x80) != 0,
          "internal-clock serial transfer starts through SB/SC");
    bus.tick(4095);
    check(bus.take_serial_output().empty() &&
              (bus.read8(0xFF02) & 0x80) != 0,
          "serial transfer remains active for its first 4095 cycles");
    bus.tick(1);
    check(bus.take_serial_output() == "A" &&
              (bus.read8(0xFF02) & 0x80) == 0 &&
              (bus.read8(0xFF0F) & 0x08) != 0,
          "serial transfer publishes its byte and requests interrupt 3");
    check(bus.take_serial_output().empty(),
          "taking serial output drains the observation buffer");

    bus.write8(0xFF0F, 0);
    bus.write8(0xFF01, 'B');
    bus.write8(0xFF02, 0x80); // External clock selected.
    bus.tick(8192);
    check(bus.take_serial_output().empty() &&
              (bus.read8(0xFF02) & 0x80) != 0 &&
              (bus.read8(0xFF0F) & 0x08) == 0,
          "external-clock serial transfer waits for an external peer");
}

void test_cpu_state_normalization() {
    auto registers = initial_registers();
    registers.f = 0xFF;
    gameboy::Cpu cpu;
    cpu.load_registers(registers);
    check(cpu.registers().f == 0xF0, "CPU state masks unavailable flag bits");
}

void test_register_load_matrix() {
    const std::array<std::uint8_t, 8> values{
        0x11, 0x22, 0x33, 0x44, 0xC0, 0x00, 0x66, 0x77,
    };

    for (unsigned destination = 0; destination < 8; ++destination) {
        for (unsigned source = 0; source < 8; ++source) {
            if (destination == 6 && source == 6) {
                continue; // Opcode 76 is HALT, not LD (HL),(HL).
            }
            const auto opcode = static_cast<std::uint8_t>(
                0x40 | (destination << 3) | source);
            gameboy::MemoryBus bus{
                gameboy::Cartridge{test_rom({opcode})}};
            bus.write8(0xC000, values[6]);
            gameboy::Cpu cpu;
            cpu.load_registers(initial_registers());

            const auto cycles = cpu.step(bus);
            const auto label = "LD matrix opcode " + std::to_string(opcode);
            const auto actual = destination == 6
                                    ? bus.read8(0xC000)
                                    : register_value(cpu.registers(), destination);
            check(actual == values[source], label + " moves the correct value");
            check(cycles == (destination == 6 || source == 6 ? 8U : 4U),
                  label + " has the correct cycle count");
            check(cpu.registers().pc == 0x0101, label + " advances PC once");
            check(cpu.registers().f == 0xB0, label + " preserves flags");
        }
    }
}

void test_immediate_load_table() {
    struct LoadCase {
        std::uint8_t opcode;
        unsigned destination;
        unsigned cycles;
    };
    constexpr std::array<LoadCase, 8> cases{{
        {0x06, 0, 8}, {0x0E, 1, 8}, {0x16, 2, 8}, {0x1E, 3, 8},
        {0x26, 4, 8}, {0x2E, 5, 8}, {0x36, 6, 12}, {0x3E, 7, 8},
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode, 0xA5})}};
        gameboy::Cpu cpu;
        cpu.load_registers(initial_registers());
        const auto cycles = cpu.step(bus);
        const auto actual = test.destination == 6
                                ? bus.read8(0xC000)
                                : register_value(cpu.registers(), test.destination);
        check(actual == 0xA5, "LD r,d8 writes its immediate operand");
        check(cycles == test.cycles, "LD r,d8 has the correct cycle count");
        check(cpu.registers().pc == 0x0102, "LD r,d8 consumes two bytes");
        check(cpu.registers().f == 0xB0, "LD r,d8 preserves flags");
    }
}

void test_arithmetic_table() {
    struct ArithmeticCase {
        const char* name;
        std::uint8_t opcode;
        std::uint8_t a;
        std::uint8_t operand;
        std::uint8_t flags;
        std::uint8_t expected_a;
        std::uint8_t expected_flags;
    };
    constexpr std::array<ArithmeticCase, 12> cases{{
        {"ADD half carry", 0x80, 0x0F, 0x01, 0x00, 0x10, 0x20},
        {"ADD zero carry", 0x80, 0xFF, 0x01, 0x00, 0x00, 0xB0},
        {"ADC carry input", 0x88, 0x0F, 0x00, 0x10, 0x10, 0x20},
        {"ADC ignores stale flags", 0x88, 0x01, 0x01, 0xE0, 0x02, 0x00},
        {"SUB half borrow", 0x90, 0x10, 0x01, 0x00, 0x0F, 0x60},
        {"SUB full borrow", 0x90, 0x00, 0x01, 0x00, 0xFF, 0x70},
        {"SBC carry input", 0x98, 0x10, 0x00, 0x10, 0x0F, 0x60},
        {"SBC zero", 0x98, 0x01, 0x00, 0x10, 0x00, 0xC0},
        {"AND", 0xA0, 0xF0, 0x0F, 0xF0, 0x00, 0xA0},
        {"XOR", 0xA8, 0xAA, 0xAA, 0xF0, 0x00, 0x80},
        {"OR", 0xB0, 0x80, 0x01, 0xF0, 0x81, 0x00},
        {"CP", 0xB8, 0x10, 0x20, 0x00, 0x10, 0x50},
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode})}};
        auto registers = initial_registers();
        registers.a = test.a;
        registers.b = test.operand;
        registers.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(registers);

        const auto cycles = cpu.step(bus);
        check(cpu.registers().a == test.expected_a,
              std::string(test.name) + " produces the expected accumulator");
        check(cpu.registers().f == test.expected_flags,
              std::string(test.name) + " produces the expected flags");
        check(cpu.registers().pc == 0x0101,
              std::string(test.name) + " advances PC once");
        check(cycles == 4, std::string(test.name) + " takes four cycles");
    }
}

void test_immediate_arithmetic_table() {
    struct ImmediateCase {
        std::uint8_t opcode;
        std::uint8_t flags;
        std::uint8_t expected_a;
        std::uint8_t expected_flags;
    };
    constexpr std::array<ImmediateCase, 8> cases{{
        {0xC6, 0x00, 0x11, 0x00}, // ADD
        {0xCE, 0x10, 0x12, 0x00}, // ADC
        {0xD6, 0x00, 0x0F, 0x60}, // SUB
        {0xDE, 0x10, 0x0E, 0x60}, // SBC
        {0xE6, 0xF0, 0x00, 0xA0}, // AND
        {0xEE, 0xF0, 0x11, 0x00}, // XOR
        {0xF6, 0xF0, 0x11, 0x00}, // OR
        {0xFE, 0x00, 0x10, 0x60}, // CP preserves A
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode, 0x01})}};
        auto registers = initial_registers();
        registers.a = 0x10;
        registers.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(registers);
        const auto cycles = cpu.step(bus);

        check(cpu.registers().a == test.expected_a,
              "immediate ALU instruction produces the expected accumulator");
        check(cpu.registers().f == test.expected_flags,
              "immediate ALU instruction produces the expected flags");
        check(cpu.registers().pc == 0x0102,
              "immediate ALU instruction consumes two bytes");
        check(cycles == 8, "immediate ALU instruction takes eight cycles");
    }
}

void test_memory_arithmetic_table() {
    struct MemoryCase {
        std::uint8_t opcode;
        std::uint8_t a;
        std::uint8_t operand;
        std::uint8_t flags;
        std::uint8_t expected_a;
        std::uint8_t expected_flags;
    };
    constexpr std::array<MemoryCase, 8> cases{{
        {0x86, 0x01, 0x02, 0x00, 0x03, 0x00},
        {0x8E, 0x01, 0x02, 0x10, 0x04, 0x00},
        {0x96, 0x03, 0x02, 0x00, 0x01, 0x40},
        {0x9E, 0x03, 0x01, 0x10, 0x01, 0x40},
        {0xA6, 0xF0, 0x0F, 0x00, 0x00, 0xA0},
        {0xAE, 0xAA, 0xAA, 0x00, 0x00, 0x80},
        {0xB6, 0x80, 0x01, 0x00, 0x81, 0x00},
        {0xBE, 0x10, 0x20, 0x00, 0x10, 0x50},
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode})}};
        bus.write8(0xC000, test.operand);
        auto registers = initial_registers();
        registers.a = test.a;
        registers.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(registers);
        check(cpu.step(bus) == 8, "ALU A,(HL) takes eight cycles");
        check(cpu.registers().a == test.expected_a,
              "ALU A,(HL) produces the expected accumulator");
        check(cpu.registers().f == test.expected_flags,
              "ALU A,(HL) produces the expected flags");
    }
}

void test_increment_decrement_table() {
    struct IncDecCase {
        const char* name;
        std::uint8_t opcode;
        std::uint8_t input;
        std::uint8_t flags;
        std::uint8_t expected;
        std::uint8_t expected_flags;
    };
    constexpr std::array<IncDecCase, 6> cases{{
        {"INC normal", 0x04, 0x01, 0x10, 0x02, 0x10},
        {"INC half carry", 0x04, 0x0F, 0x10, 0x10, 0x30},
        {"INC wrap", 0x04, 0xFF, 0x10, 0x00, 0xB0},
        {"DEC normal", 0x05, 0x02, 0x10, 0x01, 0x50},
        {"DEC half borrow", 0x05, 0x10, 0x10, 0x0F, 0x70},
        {"DEC zero", 0x05, 0x01, 0x10, 0x00, 0xD0},
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode})}};
        auto registers = initial_registers();
        registers.b = test.input;
        registers.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(registers);
        const auto cycles = cpu.step(bus);
        check(cpu.registers().b == test.expected,
              std::string(test.name) + " produces the expected value");
        check(cpu.registers().f == test.expected_flags,
              std::string(test.name) + " produces the expected flags");
        check(cycles == 4, std::string(test.name) + " takes four cycles");
    }

    for (const auto opcode : {std::uint8_t{0x34}, std::uint8_t{0x35}}) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({opcode})}};
        bus.write8(0xC000, opcode == 0x34 ? 0x0F : 0x10);
        gameboy::Cpu cpu;
        cpu.load_registers(initial_registers());
        const auto cycles = cpu.step(bus);
        check(bus.read8(0xC000) == (opcode == 0x34 ? 0x10 : 0x0F),
              "INC/DEC (HL) updates memory");
        check(cycles == 12, "INC/DEC (HL) takes twelve cycles");
    }
}

void test_sixteen_bit_operations() {
    struct PairLoadCase {
        std::uint8_t opcode;
        std::uint16_t expected;
    };
    constexpr std::array<PairLoadCase, 4> loads{{
        {0x01, 0xA1B2}, {0x11, 0xA1B2}, {0x21, 0xA1B2}, {0x31, 0xA1B2},
    }};
    for (unsigned index = 0; index < loads.size(); ++index) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({loads[index].opcode, 0xB2, 0xA1})}};
        gameboy::Cpu cpu;
        cpu.load_registers(initial_registers());
        check(cpu.step(bus) == 12, "LD rr,d16 takes twelve cycles");
        const auto& r = cpu.registers();
        const std::array<std::uint16_t, 4> actual{
            static_cast<std::uint16_t>((r.b << 8) | r.c),
            static_cast<std::uint16_t>((r.d << 8) | r.e),
            static_cast<std::uint16_t>((r.h << 8) | r.l), r.sp,
        };
        check(actual[index] == loads[index].expected,
              "LD rr,d16 loads the selected register pair");
        check(r.pc == 0x0103, "LD rr,d16 consumes three bytes");
    }

    gameboy::MemoryBus add_bus{gameboy::Cartridge{test_rom({0x09})}};
    auto add_registers = initial_registers();
    add_registers.b = 0x00;
    add_registers.c = 0x01;
    add_registers.h = 0xFF;
    add_registers.l = 0xFF;
    add_registers.f = 0x80;
    gameboy::Cpu add_cpu;
    add_cpu.load_registers(add_registers);
    check(add_cpu.step(add_bus) == 8, "ADD HL,rr takes eight cycles");
    check(add_cpu.registers().h == 0 && add_cpu.registers().l == 0,
          "ADD HL,rr wraps at sixteen bits");
    check(add_cpu.registers().f == 0xB0,
          "ADD HL,rr sets H/C, clears N, and preserves Z");

    gameboy::MemoryBus offset_bus{
        gameboy::Cartridge{test_rom({0xF8, 0xFF})}};
    auto offset_registers = initial_registers();
    offset_registers.sp = 0x0001;
    gameboy::Cpu offset_cpu;
    offset_cpu.load_registers(offset_registers);
    check(offset_cpu.step(offset_bus) == 12, "LD HL,SP+e8 takes twelve cycles");
    check(offset_cpu.registers().h == 0 && offset_cpu.registers().l == 0,
          "LD HL,SP+e8 sign-extends negative offsets");
    check(offset_cpu.registers().f == 0x30,
          "LD HL,SP+e8 derives H/C from the low unsigned byte addition");
}

void test_special_loads_and_decimal_adjust() {
    gameboy::MemoryBus store_bus{
        gameboy::Cartridge{test_rom({0x22, 0x3A})}};
    auto registers = initial_registers();
    registers.a = 0x42;
    gameboy::Cpu cpu;
    cpu.load_registers(registers);
    check(cpu.step(store_bus) == 8, "LD (HL+),A takes eight cycles");
    check(store_bus.read8(0xC000) == 0x42, "LD (HL+),A stores A");
    check(cpu.registers().l == 0x01, "LD (HL+),A increments HL");
    store_bus.write8(0xC001, 0x99);
    check(cpu.step(store_bus) == 8, "LD A,(HL-) takes eight cycles");
    check(cpu.registers().a == 0x99 && cpu.registers().l == 0x00,
          "LD A,(HL-) loads A and decrements HL");

    struct DaaCase {
        std::uint8_t a;
        std::uint8_t flags;
        std::uint8_t expected_a;
        std::uint8_t expected_flags;
    };
    constexpr std::array<DaaCase, 4> daa_cases{{
        {0x3C, 0x00, 0x42, 0x00},
        {0x32, 0x20, 0x38, 0x00},
        {0x9A, 0x00, 0x00, 0x90},
        {0x0F, 0x60, 0x09, 0x40},
    }};
    for (const auto& test : daa_cases) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({0x27})}};
        auto state = initial_registers();
        state.a = test.a;
        state.f = test.flags;
        gameboy::Cpu daa_cpu;
        daa_cpu.load_registers(state);
        check(daa_cpu.step(bus) == 4, "DAA takes four cycles");
        check(daa_cpu.registers().a == test.expected_a,
              "DAA produces the expected BCD result");
        check(daa_cpu.registers().f == test.expected_flags,
              "DAA produces the expected flags");
    }
}

void test_addressed_loads() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({
        0xEA, 0x00, 0xC1, // LD (C100),A
        0xFA, 0x00, 0xC2, // LD A,(C200)
        0xE0, 0x80,       // LDH (FF80),A
        0xF0, 0x81,       // LDH A,(FF81)
        0xE2,             // LD (FF00+C),A
        0xF2,             // LD A,(FF00+C)
        0x08, 0x00, 0xC3, // LD (C300),SP
    })}};
    bus.write8(0xC200, 0x55);
    bus.write8(0xFF81, 0x66);
    auto state = initial_registers();
    state.a = 0x42;
    state.c = 0x82;
    state.sp = 0xBEEF;
    gameboy::Cpu cpu;
    cpu.load_registers(state);

    check(cpu.step(bus) == 16 && bus.read8(0xC100) == 0x42,
          "LD (a16),A stores through an absolute address");
    check(cpu.step(bus) == 16 && cpu.registers().a == 0x55,
          "LD A,(a16) loads through an absolute address");
    check(cpu.step(bus) == 12 && bus.read8(0xFF80) == 0x55,
          "LDH (a8),A stores in high memory");
    check(cpu.step(bus) == 12 && cpu.registers().a == 0x66,
          "LDH A,(a8) loads from high memory");
    check(cpu.step(bus) == 8 && bus.read8(0xFF82) == 0x66,
          "LD (C),A uses C as a high-memory offset");
    bus.write8(0xFF82, 0x77);
    check(cpu.step(bus) == 8 && cpu.registers().a == 0x77,
          "LD A,(C) uses C as a high-memory offset");
    check(cpu.step(bus) == 20 && bus.read16(0xC300) == 0xBEEF,
          "LD (a16),SP stores little-endian SP");
    check(cpu.registers().pc == 0x010F,
          "addressed load sequence consumes every operand byte");

    gameboy::MemoryBus sp_bus{
        gameboy::Cartridge{test_rom({0xE8, 0xFF, 0xF9})}};
    auto sp_state = initial_registers();
    sp_state.sp = 0x0001;
    sp_state.h = 0xC1;
    sp_state.l = 0x23;
    gameboy::Cpu sp_cpu;
    sp_cpu.load_registers(sp_state);
    check(sp_cpu.step(sp_bus) == 16 && sp_cpu.registers().sp == 0,
          "ADD SP,e8 sign-extends a negative offset");
    check(sp_cpu.registers().f == 0x30,
          "ADD SP,e8 sets the expected low-byte carry flags");
    check(sp_cpu.step(sp_bus) == 8 && sp_cpu.registers().sp == 0xC123,
          "LD SP,HL copies the register pair");
}

void test_stack_load_table() {
    struct StackCase {
        std::uint8_t push_opcode;
        std::uint8_t pop_opcode;
        std::uint16_t value;
        std::uint16_t expected;
    };
    constexpr std::array<StackCase, 4> cases{{
        {0xC5, 0xC1, 0x1122, 0x1122},
        {0xD5, 0xD1, 0x3344, 0x3344},
        {0xE5, 0xE1, 0xC000, 0xC000},
        {0xF5, 0xF1, 0x77BF, 0x77B0},
    }};

    for (unsigned index = 0; index < cases.size(); ++index) {
        const auto& test = cases[index];
        gameboy::MemoryBus bus{gameboy::Cartridge{
            test_rom({test.push_opcode, test.pop_opcode})}};
        auto state = initial_registers();
        state.sp = 0xD000;
        gameboy::Cpu cpu;
        cpu.load_registers(state);

        check(cpu.step(bus) == 16, "PUSH qq takes sixteen cycles");
        check(cpu.registers().sp == 0xCFFE, "PUSH qq decrements SP by two");
        check(bus.read16(0xCFFE) == (test.value & 0xFFF0U |
                                    (index == 3 ? 0U : test.value & 0x000FU)),
              "PUSH qq writes the selected pair in little-endian order");

        bus.write16(0xCFFE, test.value);
        check(cpu.step(bus) == 12, "POP qq takes twelve cycles");
        check(cpu.registers().sp == 0xD000, "POP qq increments SP by two");
        const auto& result = cpu.registers();
        const std::array<std::uint16_t, 4> actual{
            static_cast<std::uint16_t>((result.b << 8) | result.c),
            static_cast<std::uint16_t>((result.d << 8) | result.e),
            static_cast<std::uint16_t>((result.h << 8) | result.l),
            static_cast<std::uint16_t>((result.a << 8) | result.f),
        };
        check(actual[index] == test.expected,
              "POP qq restores the selected pair and normalizes AF");
    }
}

void test_conditional_branch_tables() {
    struct ConditionCase {
        std::uint8_t jr_opcode;
        std::uint8_t jp_opcode;
        std::uint8_t flags_when_true;
        std::uint8_t flags_when_false;
    };
    constexpr std::array<ConditionCase, 4> conditions{{
        {0x20, 0xC2, 0x00, 0x80}, // NZ
        {0x28, 0xCA, 0x80, 0x00}, // Z
        {0x30, 0xD2, 0x00, 0x10}, // NC
        {0x38, 0xDA, 0x10, 0x00}, // C
    }};

    for (const auto& test : conditions) {
        for (const auto taken : {false, true}) {
            gameboy::MemoryBus jr_bus{gameboy::Cartridge{
                test_rom({test.jr_opcode, 0x02})}};
            auto state = initial_registers();
            state.f = taken ? test.flags_when_true : test.flags_when_false;
            gameboy::Cpu jr_cpu;
            jr_cpu.load_registers(state);
            check(jr_cpu.step(jr_bus) == (taken ? 12U : 8U),
                  "JR cc,e8 uses taken/not-taken timing");
            check(jr_cpu.registers().pc == (taken ? 0x0104 : 0x0102),
                  "JR cc,e8 applies the selected condition");

            gameboy::MemoryBus jp_bus{gameboy::Cartridge{
                test_rom({test.jp_opcode, 0x00, 0xC0})}};
            gameboy::Cpu jp_cpu;
            jp_cpu.load_registers(state);
            check(jp_cpu.step(jp_bus) == (taken ? 16U : 12U),
                  "JP cc,a16 uses taken/not-taken timing");
            check(jp_cpu.registers().pc == (taken ? 0xC000 : 0x0103),
                  "JP cc,a16 applies the selected condition");
        }
    }

    gameboy::MemoryBus relative_bus{
        gameboy::Cartridge{test_rom({0x18, 0xFE})}};
    gameboy::Cpu relative_cpu;
    relative_cpu.load_registers(initial_registers());
    check(relative_cpu.step(relative_bus) == 12 &&
              relative_cpu.registers().pc == 0x0100,
          "JR sign-extends negative offsets");

    gameboy::MemoryBus indirect_bus{
        gameboy::Cartridge{test_rom({0xE9})}};
    gameboy::Cpu indirect_cpu;
    indirect_cpu.load_registers(initial_registers());
    check(indirect_cpu.step(indirect_bus) == 4 &&
              indirect_cpu.registers().pc == 0xC000,
          "JP HL jumps without reading an immediate operand");
}

void test_calls_returns_and_restarts() {
    auto rom = test_rom({0xCD, 0x00, 0xC0});
    gameboy::MemoryBus bus{gameboy::Cartridge{std::move(rom)}};
    bus.write8(0xC000, 0xC9);
    auto state = initial_registers();
    state.sp = 0xD000;
    gameboy::Cpu cpu;
    cpu.load_registers(state);
    check(cpu.step(bus) == 24, "CALL a16 takes twenty-four cycles");
    check(cpu.registers().pc == 0xC000 && cpu.registers().sp == 0xCFFE,
          "CALL a16 jumps and reserves a stack word");
    check(bus.read16(0xCFFE) == 0x0103,
          "CALL a16 pushes the return address");
    check(cpu.step(bus) == 16 && cpu.registers().pc == 0x0103,
          "RET restores the return address");
    check(cpu.registers().sp == 0xD000, "RET releases its stack word");

    struct CallCase {
        std::uint8_t call_opcode;
        std::uint8_t return_opcode;
        std::uint8_t flags_when_true;
        std::uint8_t flags_when_false;
    };
    constexpr std::array<CallCase, 4> conditions{{
        {0xC4, 0xC0, 0x00, 0x80}, {0xCC, 0xC8, 0x80, 0x00},
        {0xD4, 0xD0, 0x00, 0x10}, {0xDC, 0xD8, 0x10, 0x00},
    }};
    for (const auto& test : conditions) {
        for (const auto taken : {false, true}) {
            gameboy::MemoryBus call_bus{gameboy::Cartridge{
                test_rom({test.call_opcode, 0x00, 0xC0})}};
            auto call_state = initial_registers();
            call_state.sp = 0xD000;
            call_state.f = taken ? test.flags_when_true : test.flags_when_false;
            gameboy::Cpu call_cpu;
            call_cpu.load_registers(call_state);
            check(call_cpu.step(call_bus) == (taken ? 24U : 12U),
                  "CALL cc,a16 uses taken/not-taken timing");
            check(call_cpu.registers().pc == (taken ? 0xC000 : 0x0103),
                  "CALL cc,a16 applies the selected condition");

            gameboy::MemoryBus return_bus{gameboy::Cartridge{
                test_rom({test.return_opcode})}};
            auto return_state = call_state;
            return_state.sp = 0xCFFE;
            return_bus.write16(0xCFFE, 0xC123);
            gameboy::Cpu return_cpu;
            return_cpu.load_registers(return_state);
            check(return_cpu.step(return_bus) == (taken ? 20U : 8U),
                  "RET cc uses taken/not-taken timing");
            check(return_cpu.registers().pc == (taken ? 0xC123 : 0x0101),
                  "RET cc applies the selected condition");
        }
    }

    constexpr std::array<std::uint8_t, 8> restarts{
        0xC7, 0xCF, 0xD7, 0xDF, 0xE7, 0xEF, 0xF7, 0xFF,
    };
    for (const auto opcode : restarts) {
        gameboy::MemoryBus rst_bus{
            gameboy::Cartridge{test_rom({opcode})}};
        auto rst_state = initial_registers();
        rst_state.sp = 0xD000;
        gameboy::Cpu rst_cpu;
        rst_cpu.load_registers(rst_state);
        check(rst_cpu.step(rst_bus) == 16, "RST vec takes sixteen cycles");
        check(rst_cpu.registers().pc == (opcode & 0x38),
              "RST vec selects the encoded vector");
        check(rst_bus.read16(0xCFFE) == 0x0101,
              "RST vec pushes the return address");
    }
}

void test_interrupt_and_low_power_states() {
    auto interrupt_rom = test_rom({0xFB, 0x00, 0x00});
    interrupt_rom[0x0040] = 0xD9; // RETI
    gameboy::MemoryBus interrupt_bus{
        gameboy::Cartridge{std::move(interrupt_rom)}};
    interrupt_bus.write8(0xFFFF, 0x01);
    interrupt_bus.write8(0xFF0F, 0x01);
    auto state = initial_registers();
    state.sp = 0xD000;
    gameboy::Cpu interrupt_cpu;
    interrupt_cpu.load_registers(state);

    check(interrupt_cpu.step(interrupt_bus) == 4 &&
              !interrupt_cpu.interrupts_enabled(),
          "EI does not enable interrupts immediately");
    check(interrupt_cpu.step(interrupt_bus) == 4 &&
              interrupt_cpu.interrupts_enabled(),
          "EI enables interrupts after the following instruction");
    check(interrupt_cpu.step(interrupt_bus) == 20,
          "interrupt dispatch takes twenty cycles");
    check(interrupt_cpu.registers().pc == 0x0040 &&
              interrupt_cpu.registers().sp == 0xCFFE,
          "interrupt dispatch pushes PC and selects the highest-priority vector");
    check(interrupt_bus.read16(0xCFFE) == 0x0102 &&
              (interrupt_bus.read8(0xFF0F) & 1) == 0,
          "interrupt dispatch stores PC and acknowledges IF");
    check(interrupt_cpu.step(interrupt_bus) == 16 &&
              interrupt_cpu.registers().pc == 0x0102 &&
              interrupt_cpu.interrupts_enabled(),
          "RETI restores PC and enables interrupts");
    check(interrupt_cpu.total_cycles() == 44,
          "CPU accumulates instruction, idle, and interrupt cycles");

    gameboy::MemoryBus di_bus{
        gameboy::Cartridge{test_rom({0xFB, 0xF3, 0x00})}};
    di_bus.write8(0xFFFF, 1);
    di_bus.write8(0xFF0F, 1);
    gameboy::Cpu di_cpu;
    di_cpu.load_registers(initial_registers());
    static_cast<void>(di_cpu.step(di_bus));
    static_cast<void>(di_cpu.step(di_bus));
    check(!di_cpu.interrupts_enabled(), "DI cancels a pending EI enable");
    check(di_cpu.step(di_bus) == 4 && di_cpu.registers().pc == 0x0103,
          "DI prevents pending interrupts from dispatching");

    gameboy::MemoryBus halt_bus{
        gameboy::Cartridge{test_rom({0x76, 0x00})}};
    gameboy::Cpu halt_cpu;
    halt_cpu.load_registers(initial_registers());
    check(halt_cpu.step(halt_bus) == 4 && halt_cpu.halted(),
          "HALT enters the halted state");
    check(halt_cpu.step(halt_bus) == 4 && halt_cpu.registers().pc == 0x0101,
          "HALT idles without advancing PC");
    halt_bus.write8(0xFFFF, 1);
    halt_bus.write8(0xFF0F, 1);
    check(halt_cpu.step(halt_bus) == 4 && !halt_cpu.halted() &&
              halt_cpu.registers().pc == 0x0102,
          "a pending interrupt wakes HALT even when IME is clear");

    gameboy::MemoryBus bug_bus{
        gameboy::Cartridge{test_rom({0x76, 0x00, 0x00})}};
    bug_bus.write8(0xFFFF, 1);
    bug_bus.write8(0xFF0F, 1);
    gameboy::Cpu bug_cpu;
    bug_cpu.load_registers(initial_registers());
    static_cast<void>(bug_cpu.step(bug_bus));
    check(!bug_cpu.halted(), "HALT bug does not enter the halted state");
    static_cast<void>(bug_cpu.step(bug_bus));
    check(bug_cpu.registers().pc == 0x0101,
          "HALT bug suppresses the next opcode-fetch increment");
    static_cast<void>(bug_cpu.step(bug_bus));
    check(bug_cpu.registers().pc == 0x0102,
          "execution resumes normally after the HALT bug fetch");

    gameboy::MemoryBus stop_bus{
        gameboy::Cartridge{test_rom({0x10, 0x00, 0x00})}};
    gameboy::Cpu stop_cpu;
    stop_cpu.load_registers(initial_registers());
    check(stop_cpu.step(stop_bus) == 4 && stop_cpu.stopped() &&
              stop_cpu.registers().pc == 0x0102,
          "STOP consumes its padding byte and enters stopped state");
    check(stop_cpu.step(stop_bus) == 4 && stop_cpu.registers().pc == 0x0102,
          "STOP idles without advancing PC");
}

void test_base_rotate_table() {
    struct RotateCase {
        std::uint8_t opcode;
        std::uint8_t value;
        std::uint8_t flags;
        std::uint8_t expected;
        std::uint8_t expected_flags;
    };
    constexpr std::array<RotateCase, 4> cases{{
        {0x07, 0x80, 0xF0, 0x01, 0x10},
        {0x0F, 0x01, 0xF0, 0x80, 0x10},
        {0x17, 0x80, 0x10, 0x01, 0x10},
        {0x1F, 0x01, 0x10, 0x80, 0x10},
    }};
    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode})}};
        auto state = initial_registers();
        state.a = test.value;
        state.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(state);
        check(cpu.step(bus) == 4, "base accumulator rotate takes four cycles");
        check(cpu.registers().a == test.expected &&
                  cpu.registers().f == test.expected_flags,
              "base accumulator rotate produces the expected result and flags");
    }
}

void test_cb_rotate_shift_table() {
    struct CbCase {
        std::uint8_t opcode;
        std::uint8_t value;
        std::uint8_t flags;
        std::uint8_t expected;
        std::uint8_t expected_flags;
    };
    constexpr std::array<CbCase, 8> cases{{
        {0x00, 0x81, 0x00, 0x03, 0x10}, // RLC
        {0x08, 0x01, 0x00, 0x80, 0x10}, // RRC
        {0x10, 0x80, 0x10, 0x01, 0x10}, // RL
        {0x18, 0x01, 0x10, 0x80, 0x10}, // RR
        {0x20, 0x81, 0x00, 0x02, 0x10}, // SLA
        {0x28, 0x81, 0x00, 0xC0, 0x10}, // SRA
        {0x30, 0xF0, 0xF0, 0x0F, 0x00}, // SWAP
        {0x38, 0x01, 0x00, 0x00, 0x90}, // SRL
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({0xCB, test.opcode})}};
        auto state = initial_registers();
        state.b = test.value;
        state.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(state);
        check(cpu.step(bus) == 8, "CB register rotate/shift takes eight cycles");
        check(cpu.registers().b == test.expected &&
                  cpu.registers().f == test.expected_flags,
              "CB register rotate/shift produces expected result and flags");

        const auto memory_opcode = static_cast<std::uint8_t>(test.opcode | 0x06);
        gameboy::MemoryBus memory_bus{
            gameboy::Cartridge{test_rom({0xCB, memory_opcode})}};
        memory_bus.write8(0xC000, test.value);
        gameboy::Cpu memory_cpu;
        memory_cpu.load_registers(state);
        check(memory_cpu.step(memory_bus) == 16,
              "CB (HL) rotate/shift takes sixteen cycles");
        check(memory_bus.read8(0xC000) == test.expected &&
                  memory_cpu.registers().f == test.expected_flags,
              "CB (HL) rotate/shift updates memory and flags");
    }
}

void test_cb_bit_reset_set_matrix() {
    for (unsigned bit = 0; bit < 8; ++bit) {
        const auto mask = static_cast<std::uint8_t>(1U << bit);
        for (const auto target : {0U, 6U}) {
            const auto bit_opcode = static_cast<std::uint8_t>(
                0x40 | (bit << 3) | target);
            gameboy::MemoryBus bit_bus{
                gameboy::Cartridge{test_rom({0xCB, bit_opcode})}};
            auto state = initial_registers();
            state.b = mask;
            state.f = 0x10;
            bit_bus.write8(0xC000, mask);
            gameboy::Cpu bit_cpu;
            bit_cpu.load_registers(state);
            check(bit_cpu.step(bit_bus) == (target == 6 ? 12U : 8U),
                  "BIT b,r uses register/(HL) timing");
            check(bit_cpu.registers().f == 0x30,
                  "BIT b,r preserves C, sets H, and reports a set bit");

            const auto res_opcode = static_cast<std::uint8_t>(
                0x80 | (bit << 3) | target);
            gameboy::MemoryBus res_bus{
                gameboy::Cartridge{test_rom({0xCB, res_opcode})}};
            res_bus.write8(0xC000, 0xFF);
            state.b = 0xFF;
            state.f = 0xB0;
            gameboy::Cpu res_cpu;
            res_cpu.load_registers(state);
            check(res_cpu.step(res_bus) == (target == 6 ? 16U : 8U),
                  "RES b,r uses register/(HL) timing");
            const auto res_value = target == 6 ? res_bus.read8(0xC000)
                                               : res_cpu.registers().b;
            check(res_value == static_cast<std::uint8_t>(0xFF & ~mask) &&
                      res_cpu.registers().f == 0xB0,
                  "RES b,r clears the selected bit and preserves flags");

            const auto set_opcode = static_cast<std::uint8_t>(
                0xC0 | (bit << 3) | target);
            gameboy::MemoryBus set_bus{
                gameboy::Cartridge{test_rom({0xCB, set_opcode})}};
            set_bus.write8(0xC000, 0x00);
            state.b = 0x00;
            gameboy::Cpu set_cpu;
            set_cpu.load_registers(state);
            check(set_cpu.step(set_bus) == (target == 6 ? 16U : 8U),
                  "SET b,r uses register/(HL) timing");
            const auto set_value = target == 6 ? set_bus.read8(0xC000)
                                               : set_cpu.registers().b;
            check(set_value == mask && set_cpu.registers().f == 0xB0,
                  "SET b,r sets the selected bit and preserves flags");
        }
    }
}

void test_base_opcode_completeness() {
    const auto invalid = [](const std::uint8_t opcode) {
        switch (opcode) {
        case 0xD3: case 0xDB: case 0xDD:
        case 0xE3: case 0xE4: case 0xEB: case 0xEC: case 0xED:
        case 0xF4: case 0xFC: case 0xFD:
            return true;
        default:
            return false;
        }
    };

    for (unsigned value = 0; value <= 0xFF; ++value) {
        const auto opcode = static_cast<std::uint8_t>(value);
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({opcode, 0x00, 0xC0})}};
        auto state = initial_registers();
        state.sp = 0xD000;
        gameboy::Cpu cpu;
        cpu.load_registers(state);
        auto rejected = false;
        try {
            static_cast<void>(cpu.step(bus));
        } catch (const gameboy::UnsupportedOpcode&) {
            rejected = true;
        }
        check(rejected == invalid(opcode),
              "base decoder accepts every legal opcode and rejects only invalid ones");
    }
}

void test_divider_and_timer_frequencies() {
    gameboy::MemoryBus divider_bus{gameboy::Cartridge{test_rom()}};
    divider_bus.tick(255);
    check(divider_bus.read8(0xFF04) == 0,
          "DIV exposes the upper byte of the internal divider");
    divider_bus.tick(1);
    check(divider_bus.read8(0xFF04) == 1,
          "DIV advances once per 256 T-cycles");
    divider_bus.write8(0xFF04, 0xAB);
    check(divider_bus.read8(0xFF04) == 0,
          "writing any value to DIV resets the internal divider");

    struct FrequencyCase {
        std::uint8_t selection;
        unsigned period;
    };
    constexpr std::array<FrequencyCase, 4> frequencies{{
        {0, 1024}, {1, 16}, {2, 64}, {3, 256},
    }};
    for (const auto& test : frequencies) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
        bus.write8(0xFF07, static_cast<std::uint8_t>(0x04 | test.selection));
        check(bus.read8(0xFF07) ==
                  static_cast<std::uint8_t>(0xFC | test.selection),
              "TAC stores its low bits and reads unused bits high");
        bus.tick(test.period - 1);
        check(bus.read8(0xFF05) == 0,
              "TIMA waits for the selected divider falling edge");
        bus.tick(1);
        check(bus.read8(0xFF05) == 1,
              "TIMA increments at the selected TAC frequency");
    }

    gameboy::MemoryBus disabled_bus{gameboy::Cartridge{test_rom()}};
    disabled_bus.write8(0xFF07, 0x01);
    disabled_bus.tick(1024);
    check(disabled_bus.read8(0xFF05) == 0,
          "TIMA does not advance while TAC is disabled");
}

void test_timer_overflow_pipeline() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.write8(0xFF05, 0xFF);
    bus.write8(0xFF06, 0x42);
    bus.write8(0xFF07, 0x05);
    bus.tick(16);
    check(bus.read8(0xFF05) == 0 && (bus.read8(0xFF0F) & 0x04) == 0,
          "TIMA overflow enters its four-cycle pending state");
    bus.tick(3);
    check(bus.read8(0xFF05) == 0 && (bus.read8(0xFF0F) & 0x04) == 0,
          "TIMA remains zero during the reload delay");
    bus.tick(1);
    check(bus.read8(0xFF05) == 0x42 && (bus.read8(0xFF0F) & 0x04) != 0,
          "TIMA reloads TMA and requests interrupt 2 after four cycles");

    gameboy::MemoryBus cancel_bus{gameboy::Cartridge{test_rom()}};
    cancel_bus.write8(0xFF05, 0xFF);
    cancel_bus.write8(0xFF06, 0x42);
    cancel_bus.write8(0xFF07, 0x05);
    cancel_bus.tick(16);
    cancel_bus.write8(0xFF05, 0x99);
    cancel_bus.tick(4);
    check(cancel_bus.read8(0xFF05) == 0x99 &&
              (cancel_bus.read8(0xFF0F) & 0x04) == 0,
          "writing TIMA during overflow cancels reload and interrupt");

    gameboy::MemoryBus modulo_bus{gameboy::Cartridge{test_rom()}};
    modulo_bus.write8(0xFF05, 0xFF);
    modulo_bus.write8(0xFF06, 0x42);
    modulo_bus.write8(0xFF07, 0x05);
    modulo_bus.tick(16);
    modulo_bus.tick(2);
    modulo_bus.write8(0xFF06, 0x55);
    modulo_bus.tick(2);
    check(modulo_bus.read8(0xFF05) == 0x55,
          "a TMA write during overflow changes the pending reload value");
}

void test_timer_write_edges() {
    gameboy::MemoryBus divider_bus{gameboy::Cartridge{test_rom()}};
    divider_bus.write8(0xFF07, 0x05);
    divider_bus.tick(8); // Selected divider bit 3 is now high.
    divider_bus.write8(0xFF04, 0);
    check(divider_bus.read8(0xFF05) == 1,
          "resetting DIV across a timer-input falling edge increments TIMA");

    gameboy::MemoryBus control_bus{gameboy::Cartridge{test_rom()}};
    control_bus.write8(0xFF07, 0x05);
    control_bus.tick(8);
    control_bus.write8(0xFF07, 0x00);
    check(control_bus.read8(0xFF05) == 1,
          "changing TAC across a timer-input falling edge increments TIMA");
}

void test_emulator_timer_integration() {
    gameboy::Emulator emulator{
        gameboy::Cartridge{test_rom({0x00, 0x00, 0x00, 0x00})}};
    emulator.bus().write8(0xFF07, 0x05);
    for (unsigned instruction = 0; instruction < 4; ++instruction) {
        check(emulator.step() == 4, "NOP takes four cycles during timer integration");
    }
    check(emulator.bus().read8(0xFF05) == 1,
          "emulator steps forward timer hardware by CPU cycles");

    gameboy::Emulator halted{
        gameboy::Cartridge{test_rom({0x76, 0x00})}};
    halted.bus().write8(0xFFFF, 0x04);
    halted.bus().write8(0xFF05, 0xFF);
    halted.bus().write8(0xFF06, 0x00);
    halted.bus().write8(0xFF07, 0x05);
    for (unsigned step = 0; step < 5; ++step) {
        static_cast<void>(halted.step());
    }
    check(halted.cpu().halted() && (halted.bus().read8(0xFF0F) & 0x04) != 0,
          "timer continues running and requests interrupts while CPU is halted");
    check(halted.step() == 4 && !halted.cpu().halted() &&
              halted.cpu().registers().pc == 0x0102,
          "a timer interrupt wakes HALT when IME is disabled");

    gameboy::Emulator stopped{
        gameboy::Cartridge{test_rom({0x10, 0x00, 0x00})}};
    stopped.bus().tick(300);
    check(stopped.bus().read8(0xFF04) == 1,
          "STOP integration begins with an advanced divider");
    static_cast<void>(stopped.step());
    check(stopped.cpu().stopped() && stopped.bus().read8(0xFF04) == 0,
          "STOP resets the system divider");
    for (unsigned idle = 0; idle < 100; ++idle) {
        static_cast<void>(stopped.step());
    }
    check(stopped.bus().read8(0xFF04) == 0,
          "the system divider remains frozen while STOP is active");
}

void test_ppu_modes_and_memory_access() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.write8(0x8000, 0x12);
    bus.write8(0xFE00, 0x34);
    bus.write8(0xFF40, 0x80);
    check((bus.read8(0xFF41) & 0x03) == 2 && bus.read8(0xFF44) == 0,
          "enabling LCD starts line zero in OAM scan mode");
    check(bus.read8(0x8000) == 0x12,
          "VRAM remains accessible during mode 2");
    check(bus.read8(0xFE00) == 0xFF,
          "OAM is blocked during mode 2");

    bus.tick(79);
    check((bus.read8(0xFF41) & 0x03) == 2,
          "mode 2 lasts eighty dots");
    bus.tick(1);
    check((bus.read8(0xFF41) & 0x03) == 3,
          "mode 3 begins on dot eighty");
    check(bus.read8(0x8000) == 0xFF && bus.read8(0xFE00) == 0xFF,
          "VRAM and OAM are blocked during mode 3");
    bus.write8(0x8000, 0x99);

    bus.tick(172);
    check((bus.read8(0xFF41) & 0x03) == 0,
          "minimum-length mode 3 ends on dot 252");
    check(bus.read8(0x8000) == 0x12,
          "writes to VRAM during mode 3 are ignored");
    check(bus.read8(0xFE00) == 0x34,
          "OAM becomes accessible during HBlank");

    bus.tick(204);
    check(bus.read8(0xFF44) == 1 && (bus.read8(0xFF41) & 0x03) == 2,
          "a 456-dot visible line advances LY and returns to mode 2");
    bus.write8(0xFF44, 99);
    check(bus.read8(0xFF44) == 1, "LY ignores CPU writes");

    bus.write8(0xFF40, 0);
    check(bus.read8(0xFF44) == 0 && (bus.read8(0xFF41) & 0x03) == 0,
          "disabling LCD resets LY and reports mode 0");
}

void test_ppu_stat_interrupts() {
    gameboy::MemoryBus coincidence{gameboy::Cartridge{test_rom()}};
    coincidence.write8(0xFF45, 1);
    coincidence.write8(0xFF41, 0x40);
    coincidence.write8(0xFF40, 0x80);
    coincidence.write8(0xFF0F, 0);
    coincidence.tick(456);
    check((coincidence.read8(0xFF41) & 0x04) != 0 &&
              (coincidence.read8(0xFF0F) & 0x02) != 0,
          "LY=LYC raises the coincidence flag and STAT interrupt");

    gameboy::MemoryBus modes{gameboy::Cartridge{test_rom()}};
    modes.write8(0xFF41, 0x28); // Mode 2 and mode 0 interrupt sources.
    modes.write8(0xFF40, 0x80);
    check((modes.read8(0xFF0F) & 0x02) != 0,
          "enabling LCD in mode 2 can raise STAT");
    modes.write8(0xFF0F, 0);
    modes.tick(80);
    modes.tick(172);
    check((modes.read8(0xFF0F) & 0x02) != 0,
          "entering enabled mode 0 raises STAT on a rising edge");
    modes.write8(0xFF0F, 0);
    modes.tick(204);
    check((modes.read8(0xFF0F) & 0x02) == 0,
          "adjacent enabled STAT sources block a second interrupt edge");
}

void test_ppu_vblank_and_frame_publication() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.write8(0xFF41, 0x10); // Mode 1 STAT source.
    bus.write8(0xFF40, 0x80);
    bus.write8(0xFF0F, 0);
    bus.tick(456 * 144);
    check(bus.read8(0xFF44) == 144 && (bus.read8(0xFF41) & 0x03) == 1,
          "line 144 begins VBlank mode");
    check((bus.read8(0xFF0F) & 0x03) == 0x03,
          "entering VBlank requests VBlank and enabled mode-1 STAT interrupts");
    check(bus.frame_ready(), "entering VBlank publishes the completed frame");
    bus.consume_frame();
    check(!bus.frame_ready(), "frontend can acknowledge a published frame");
    bus.tick(456 * 10);
    check(bus.read8(0xFF44) == 0 && (bus.read8(0xFF41) & 0x03) == 2,
          "ten VBlank lines wrap LY to zero and begin mode 2");
}

void test_ppu_background_window_and_sprites() {
    gameboy::MemoryBus background{gameboy::Cartridge{test_rom()}};
    background.write8(0xFF47, 0xE4); // Identity DMG palette.
    background.write8(0x8000, 0x80);
    background.write8(0x8001, 0x80); // Tile 0, first pixel color 3.
    background.write8(0x9800, 0x00);
    background.write8(0xFF40, 0x91);
    background.tick(252);
    check(background.framebuffer()[0] == 0xFF000000 &&
              background.framebuffer()[1] == 0xFFFFFFFF,
          "background tile data renders through BGP into the framebuffer");

    gameboy::MemoryBus window{gameboy::Cartridge{test_rom()}};
    window.write8(0xFF47, 0xE4);
    window.write8(0x8010, 0xFF);
    window.write8(0x8011, 0x00); // Tile 1 row is color 1.
    window.write8(0x9C00, 0x01);
    window.write8(0xFF4A, 0);
    window.write8(0xFF4B, 7);
    window.write8(0xFF40, 0xF1);
    window.tick(252);
    check(window.framebuffer()[0] == 0xFFAAAAAA,
          "enabled window uses WX/WY and its selected tile map");

    gameboy::MemoryBus sprites{gameboy::Cartridge{test_rom()}};
    sprites.write8(0xFF47, 0xE4);
    sprites.write8(0xFF48, 0xE4);
    sprites.write8(0x8010, 0x00);
    sprites.write8(0x8011, 0x80); // Tile 1, first pixel color 2.
    sprites.write8(0xFE00, 16);   // Screen Y = 0.
    sprites.write8(0xFE01, 8);    // Screen X = 0.
    sprites.write8(0xFE02, 1);
    sprites.write8(0xFE03, 0);
    sprites.write8(0xFF40, 0x93);
    sprites.tick(252);
    check(sprites.framebuffer()[0] == 0xFF555555,
          "visible OBJ pixels render with their selected DMG palette");
}

void test_joypad_matrix_and_interrupts() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    check(bus.read8(0xFF00) == 0xFF,
          "joypad starts with neither input group selected");

    bus.set_button(gameboy::Button::a, true);
    check(bus.read8(0xFF00) == 0xFF && (bus.read8(0xFF0F) & 0x10) == 0,
          "an unselected pressed button does not pull an input line low");
    bus.write8(0xFF00, 0x10);
    check(bus.read8(0xFF00) == 0xDE,
          "action selection exposes A on input line zero");
    check((bus.read8(0xFF0F) & 0x10) != 0,
          "selecting an already-pressed button requests the joypad interrupt");

    bus.write8(0xFF0F, 0);
    bus.set_button(gameboy::Button::a, false);
    check((bus.read8(0xFF0F) & 0x10) == 0,
          "button release does not request a joypad interrupt");
    bus.set_button(gameboy::Button::start, true);
    check(bus.read8(0xFF00) == 0xD7 && (bus.read8(0xFF0F) & 0x10) != 0,
          "selected action press updates FF00 and requests interrupt 4");

    bus.write8(0xFF0F, 0);
    bus.write8(0xFF00, 0x20);
    bus.set_button(gameboy::Button::right, true);
    bus.set_button(gameboy::Button::up, true);
    check(bus.read8(0xFF00) == 0xEA,
          "direction selection exposes active-low directional lines");
    check((bus.read8(0xFF0F) & 0x10) != 0,
          "selected direction presses request the joypad interrupt");

    bus.write8(0xFF00, 0x00);
    check((bus.read8(0xFF00) & 0x0F) == 0x02,
          "selecting both groups combines their active-low input lines");
}

void test_oam_dma() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    for (unsigned offset = 0; offset < 0xA0; ++offset) {
        bus.write8(static_cast<std::uint16_t>(0xC000 + offset),
                   static_cast<std::uint8_t>(offset));
    }
    bus.write8(0xFF46, 0xC0);
    for (unsigned offset = 0; offset < 0xA0; ++offset) {
        check(bus.read8(static_cast<std::uint16_t>(0xFE00 + offset)) ==
                  static_cast<std::uint8_t>(offset),
              "OAM DMA copies all 160 source bytes");
    }
    check(bus.read8(0xFF46) == 0xC0, "DMA register retains its source page");
}

} // namespace

int main() {
    try {
        test_cartridge_header();
        test_cartridge_file_loading();
        test_mbc1_rom_banking();
        test_mbc1_ram_banking();
        test_battery_ram_persistence();
        test_mbc3_banking_and_rtc();
        test_mbc3_rtc_persistence();
        test_mbc5_banking_and_rumble();
        test_memory_map();
        test_apu_power_registers_and_wave_ram();
        test_apu_high_pass_filter();
        test_apu_pulse2_samples_and_length();
        test_apu_pulse1_sweep_wave_and_noise();
        test_serial_transfer();
        test_cpu_state_normalization();
        test_register_load_matrix();
        test_immediate_load_table();
        test_arithmetic_table();
        test_immediate_arithmetic_table();
        test_memory_arithmetic_table();
        test_increment_decrement_table();
        test_sixteen_bit_operations();
        test_special_loads_and_decimal_adjust();
        test_addressed_loads();
        test_stack_load_table();
        test_conditional_branch_tables();
        test_calls_returns_and_restarts();
        test_interrupt_and_low_power_states();
        test_base_rotate_table();
        test_cb_rotate_shift_table();
        test_cb_bit_reset_set_matrix();
        test_base_opcode_completeness();
        test_divider_and_timer_frequencies();
        test_timer_overflow_pipeline();
        test_timer_write_edges();
        test_emulator_timer_integration();
        test_ppu_modes_and_memory_access();
        test_ppu_stat_interrupts();
        test_ppu_vblank_and_frame_publication();
        test_ppu_background_window_and_sprites();
        test_joypad_matrix_and_interrupts();
        test_oam_dma();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }

    if (failures == 0) {
        std::cout << "All tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
