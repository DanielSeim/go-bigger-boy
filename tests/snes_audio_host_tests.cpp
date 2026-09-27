#include "gameboy/snes_audio_host.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

int failures{};

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> synthetic_program(const std::size_t size,
                                             const std::uint8_t map_mode) {
    std::vector<std::uint8_t> image(size);
    image[0x7FD5] = map_mode;
    image[0x7FFC] = 0x04;
    image[0x7FFD] = 0x81;
    image[0x0104] = 0xA5;
    image[0x8104] = 0x5A;
    return image;
}

void test_program_mapping() {
    auto image = synthetic_program(0x40000, 0x20);
    check(gameboy::SgbProgramRom::validate(image) ==
              gameboy::SgbProgramRom::Error::none,
          "SGB1-size LoROM with high reset vector is supported");
    gameboy::SgbProgramRom rom(image);
    check(rom.size() == 0x40000 && rom.reset_vector() == 0x8104 &&
              rom.read(0, 0x8104) == 0xA5 &&
              rom.read(1, 0x8104) == 0x5A &&
              rom.read(0x80, 0x8104) == 0xA5,
          "LoROM high-half mapping and mirrored upper banks are deterministic");
    check(rom.read(0, 0x0104) == 0xFF && rom.read(0x7E, 0x8104) == 0xFF,
          "lower-half I/O and WRAM banks are not mistaken for cartridge ROM");
    check(rom.read(0xFE, 0x8104) != 0xFF,
          "upper FE/FF cartridge banks are not mistaken for WRAM banks");
    image[0x40000 - 1] = 0x44;
    gameboy::SgbProgramRom wrapped(image);
    check(wrapped.read(7, 0xFFFF) == 0x44 &&
              wrapped.read(15, 0xFFFF) == 0x44,
          "program banks mirror over the physical ROM size");

    image = synthetic_program(0x80000, 0x30);
    check(gameboy::SgbProgramRom::validate(image) ==
              gameboy::SgbProgramRom::Error::none,
          "SGB2-size Fast LoROM is supported");
    image[0x7FD5] = 0x21;
    check(gameboy::SgbProgramRom::validate(image) ==
              gameboy::SgbProgramRom::Error::unsupported_mapping,
          "HiROM is not silently interpreted as LoROM");
    image = synthetic_program(0x40000, 0x20);
    image[0x7FFD] = 0x40;
    check(gameboy::SgbProgramRom::validate(image) ==
              gameboy::SgbProgramRom::Error::invalid_reset_vector,
          "ROM without an executable reset vector is rejected");
    image.resize(0x8000);
    check(gameboy::SgbProgramRom::validate(image) ==
              gameboy::SgbProgramRom::Error::unsupported_size,
          "small or incomplete firmware is rejected");
}

void test_apu_bus() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xA9;
    ipl[63] = 0x55;
    check(!bus.has_ipl(), "no proprietary IPL is built into the host");
    bus.install_ipl(ipl);
    check(bus.has_ipl() && bus.spc_read(0xFFC0) == 0xA9 &&
              bus.spc_read(0xFFFF) == 0x55,
          "user-supplied IPL overlays the top 64 bytes");
    bus.spc_write(0xFFC0, 0x12);
    check(bus.spc_read(0xFFC0) == 0xA9,
          "RAM remains hidden by an enabled IPL overlay");
    bus.spc_write(0xF1, 0x00);
    check(bus.spc_read(0xFFC0) == 0x12,
          "disabling IPL exposes previously written underlying RAM");
    bus.spc_write(0xF1, 0x80);
    check(bus.spc_read(0xFFC0) == 0xA9,
          "IPL overlay can be enabled again");

    bus.host_write_port(0, 0x34);
    bus.host_write_port(1, 0x56);
    check(bus.spc_read(0xF4) == 0x34 && bus.spc_read(0xF5) == 0x56,
          "host-to-SPC ports are visible to the sound CPU");
    bus.spc_write(0xF4, 0x78);
    check(bus.host_read_port(0) == 0x78 && bus.spc_read(0xF4) == 0x34,
          "SPC-to-host ports are independent of host-to-SPC ports");
    bus.spc_write(0xF1, 0x10);
    check(bus.spc_read(0xF4) == 0 && bus.spc_read(0xF5) == 0 &&
              bus.host_read_port(0) == 0x78,
          "CONTROL clears only the requested host-to-SPC port pair");
    bus.spc_write(0xF2, 0x8C);
    bus.spc_write(0xF3, 0x9A);
    check(bus.dsp_register(0x0C) == 0,
          "writes through DSP addresses above 0x7F are ignored");
    bus.spc_write(0xF2, 0x0C);
    bus.spc_write(0xF3, 0x9A);
    check(bus.dsp_register(0x0C) == 0x9A && bus.spc_read(0xF3) == 0x9A,
          "DSP address/data register window selects one of 128 registers");
    bus.spc_write(0xF1, 0x80); // re-enable IPL after the earlier port-clear test
    bus.dsp_write_ram(0xF3, 0x55);
    bus.dsp_write_ram(0xFFC0, 0x66);
    check(bus.dsp_read_ram(0xF3) == 0x55 &&
              bus.dsp_read_ram(0xFFC0) == 0x66,
          "DSP echo writes update physical RAM beneath I/O and IPL overlays");
    check(bus.dsp_register(0x0C) == 0x9A && bus.spc_read(0xF3) == 0x9A,
          "physical echo writes do not change the SPC700 DSP data port");
    check(bus.spc_read(0xFFC0) == 0xA9,
          "physical echo writes do not disable or replace the IPL overlay");

    bus.spc_write(0xFA, 2);
    bus.spc_write(0xF1, 0x01);
    bus.tick(127);
    check(bus.spc_read(0xFD) == 0, "timer 0 does not tick early");
    bus.tick(1);
    check(bus.spc_read(0xFD) == 0, "one stage-1 tick is below target two");
    bus.tick(128);
    check(bus.spc_read(0xFD) == 1 && bus.spc_read(0xFD) == 0,
          "timer 0 advances every 128 clocks and its output clears on read");
    bus.spc_write(0xF1, 0);
    bus.tick(64);
    bus.spc_write(0xF1, 1);
    bus.tick(63);
    check(bus.spc_read(0xFD) == 0,
          "enabling a timer does not reset the free-running stage-1 phase");
    bus.tick(1);
    bus.tick(128);
    check(bus.spc_read(0xFD) == 1,
          "timer stage 2 and 3 restart when CONTROL re-enables the timer");
    bus.spc_write(0xFC, 0); // zero target means 256, not zero clocks
    bus.spc_write(0xF1, 0x04);
    bus.tick(16 * 255);
    check(bus.spc_read(0xFF) == 0,
          "timer 2 target zero requires 256 stage-1 ticks");
    bus.tick(16);
    check(bus.spc_read(0xFF) == 1,
          "timer 2 runs at 16-clock intervals and wraps target zero");
    bus.reset();
    check(bus.has_ipl() && bus.spc_read(0xFFC0) == 0xA9 &&
              bus.host_read_port(0) == 0 && bus.dsp_register(0x0C) == 0,
          "reset clears runtime state without discarding supplied IPL image");
}

void test_timer_batching() {
    // Compare the fast bulk advance to a literal single-cycle reference
    // across target changes, enable transitions, and fractional phases.
    gameboy::SnesApuBus bulk;
    gameboy::SnesApuBus stepped;
    std::uint32_t random = 0x53474232U;
    const auto next = [&random]() {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return random;
    };
    for (unsigned iteration = 0; iteration < 200; ++iteration) {
        const auto control = static_cast<std::uint8_t>(next() & 7U);
        bulk.spc_write(0xF1, control);
        stepped.spc_write(0xF1, control);
        for (unsigned timer = 0; timer < 3; ++timer) {
            const auto target = static_cast<std::uint8_t>(next() & 0xFFU);
            bulk.spc_write(static_cast<std::uint16_t>(0xFA + timer), target);
            stepped.spc_write(static_cast<std::uint16_t>(0xFA + timer), target);
        }
        const auto cycles = (next() % 2600U) + 1U;
        bulk.tick(cycles);
        for (unsigned cycle = 0; cycle < cycles; ++cycle) stepped.tick(1);
        for (unsigned timer = 0; timer < 3; ++timer) {
            const auto address = static_cast<std::uint16_t>(0xFD + timer);
            check(bulk.spc_read(address) == stepped.spc_read(address),
                  "batched timers agree with one-cycle stepping");
        }
    }

    bulk.reset();
    bulk.spc_write(0xFC, 1);
    bulk.spc_write(0xF1, 4);
    bulk.tick(16U * 1'000'000U);
    check(bulk.spc_read(0xFF) == (1'000'000U & 15U),
          "large timer advances keep only the hardware four-bit output");
}

void test_local_programs(const int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        try {
            const auto rom = gameboy::SgbProgramRom::from_file(argv[index]);
            check(rom.reset_vector() >= 0x8000,
                  "local SGB program exposes an executable reset vector");
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << argv[index] << ": " << error.what() << '\n';
            ++failures;
        }
    }
}

} // namespace

int main(const int argc, char** argv) {
    test_program_mapping();
    test_apu_bus();
    test_timer_batching();
    test_local_programs(argc, argv);
    return failures == 0 ? 0 : 1;
}
