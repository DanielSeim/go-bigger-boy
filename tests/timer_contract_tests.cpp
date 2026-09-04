#include "gameboy/emulator.hpp"
#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
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
    constexpr std::string_view title = "TIMER TEST";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    std::copy(program.begin(), program.end(), rom.begin() + program_address);
    return rom;
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
    halted.bus().write8(0xFF04, 0);
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
    check(stopped.bus().read8(0xFF04) != 0,
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


} // namespace

int main() {
    test_divider_and_timer_frequencies();
    test_timer_overflow_pipeline();
    test_timer_write_edges();
    test_emulator_timer_integration();
    return failures == 0 ? 0 : 1;
}
