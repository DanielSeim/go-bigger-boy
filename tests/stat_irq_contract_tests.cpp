#include "gameboy/emulator.hpp"
#include "gameboy/save_state_error.hpp"
#include "save_state_container.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {
int failures{};
void check(bool result, const char* message) {
    if (!result) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

// Independent, logo-free synthetic program: enable interrupts, then NOPs.
std::vector<std::uint8_t> rom() {
    std::vector<std::uint8_t> bytes(0x8000);
    bytes[0x100] = 0xFB; // EI
    return bytes;
}

void prepare(gameboy::Emulator& emulator, unsigned lyc, unsigned edge_after,
             bool batched) {
    auto& bus = emulator.bus();
    bus.debug_set_peripheral_batch_enabled(batched);
    bus.write8(0xFF40, 0);
    bus.write8(0xFF0F, 0);
    bus.write8(0xFFFF, 2);
    check(emulator.step() == 4 && emulator.step() == 4,
          "EI and its following NOP keep their ordinary timing");
    bus.write8(0xFF45, static_cast<std::uint8_t>(lyc));
    bus.write8(0xFF41, 0x40);
    bus.write8(0xFF40, 0x80);
    const unsigned edge = 153U * 456U - 4U + (lyc == 153 ? 4U : 12U);
    bus.tick(edge - edge_after);
    bus.write8(0xFF0F, 2); // A distinct software-requested interrupt.
}

void test_ack_boundary() {
    for (auto model : {gameboy::HardwareModel::dmg, gameboy::HardwareModel::mgb}) {
        for (bool batched : {false, true}) {
            for (unsigned lyc : {0U, 153U}) {
                for (unsigned edge_after = 17; edge_after <= 29; ++edge_after) {
                    gameboy::Emulator emulator{gameboy::Cartridge{rom()}, model};
                    prepare(emulator, lyc, edge_after, batched);
                    check(emulator.step() == 20 && emulator.cpu().registers().pc == 0x48 &&
                              emulator.cpu().registers().sp == 0xFFFC &&
                              emulator.bus().read16(0xFFFC) == 0x102,
                          "STAT arbitration, stack writes, vector and twenty-clock dispatch stay intact");
                    check(emulator.step() == 4 && emulator.cpu().registers().pc == 0x49,
                          "first vector opcode remains an ordinary four-clock NOP");
                    // Acknowledge overlaps the vector fetch, not later instructions.
                    emulator.bus().tick(6); // Observe all edges through clock 30.
                    check(((emulator.bus().read8(0xFF0F) & 2U) != 0) == (edge_after > 24),
                          "coincident STAT edge is consumed; a later edge can retrigger");
                }
            }
        }
    }
}

void test_save_and_reset() {
    gameboy::Emulator emulator{gameboy::Cartridge{rom()}, gameboy::HardwareModel::dmg};
    prepare(emulator, 153, 24, true);
    (void)emulator.step();
    const auto pending = emulator.save_state();
    (void)emulator.step();
    const auto completed = emulator.save_state();
    emulator.load_state(pending);
    (void)emulator.step();
    check(emulator.save_state() == completed,
          "save/load between dispatch and vector fetch restores the acknowledgment phase exactly");

    auto decoded = gameboy::save_state_container::decode(pending, emulator.rom_fingerprint());
    decoded.payload.back() = 2; // Invalid boolean, with a valid container checksum.
    const auto malformed = gameboy::save_state_container::encode(
        emulator.rom_fingerprint(), decoded.payload);
    emulator.load_state(pending);
    bool rejected{};
    try { emulator.load_state(malformed); }
    catch (const gameboy::SaveStateError&) { rejected = true; }
    check(rejected && emulator.save_state() == pending,
          "malformed acknowledgment phase is rejected and rolls back atomically");

    decoded.payload.pop_back(); // The pre-extension payload is still version 40.
    auto legacy = gameboy::save_state_container::encode(
        emulator.rom_fingerprint(), decoded.payload);
    legacy[8] = 40;
    emulator.load_state(legacy);
    (void)emulator.step();
    check((emulator.bus().read8(0xFF0F) & 2U) != 0,
          "loading a legacy state clears any stale in-memory acknowledgment phase");

    for (bool cold : {false, true}) {
        gameboy::MemoryBus bus{gameboy::Cartridge{rom()}};
        gameboy::Cpu cpu;
        bus.write8(0xFF40, 0);
        bus.write8(0xFF0F, 0);
        bus.write8(0xFFFF, 2);
        (void)cpu.step(bus);
        (void)cpu.step(bus);
        bus.write8(0xFF45, 153);
        bus.write8(0xFF41, 0x40);
        bus.write8(0xFF40, 0x80);
        bus.tick(153U * 456U - 24U);
        bus.write8(0xFF0F, 2);
        (void)cpu.step(bus);
        if (cold) cpu.reset_boot();
        else cpu.reset();
        (void)cpu.step(bus);
        check((bus.read8(0xFF0F) & 2U) != 0,
              "warm and cold CPU resets cancel a pending acknowledgment");
    }
}

void test_other_interrupts_and_models() {
    for (auto model : {gameboy::HardwareModel::dmg, gameboy::HardwareModel::cgb_c,
                       gameboy::HardwareModel::sgb, gameboy::HardwareModel::sgb2}) {
        gameboy::Emulator emulator{gameboy::Cartridge{rom()}, model};
        auto& bus = emulator.bus();
        bus.write8(0xFF40, 0);
        bus.write8(0xFF0F, 0);
        bus.write8(0xFFFF, 2);
        (void)emulator.step();
        (void)emulator.step();
        bus.write8(0xFF0F, 2);
        (void)emulator.step();
        bus.write8(0xFF0F, 0x1F);
        (void)emulator.step();
        check(bus.read8(0xFF0F) == (model == gameboy::HardwareModel::dmg ? 0xFD : 0xFF),
              "STAT reset preserves other IF bits and is not applied to CGB/SGB models");
    }
    gameboy::Emulator emulator{gameboy::Cartridge{rom()}, gameboy::HardwareModel::dmg};
    auto& bus = emulator.bus();
    bus.write8(0xFF40, 0);
    bus.write8(0xFF0F, 0);
    bus.write8(0xFFFF, 3);
    (void)emulator.step();
    (void)emulator.step();
    bus.write8(0xFF0F, 3);
    (void)emulator.step();
    check(emulator.cpu().registers().pc == 0x40 && bus.read8(0xFF0F) == 0xE2,
          "VBlank retains priority over STAT and clears only the selected flag");
    bus.write8(0xFF0F, 3);
    (void)emulator.step();
    check(bus.read8(0xFF0F) == 0xE3, "VBlank dispatch does not arm a STAT acknowledgment");
}

void test_mixed_stat_timer_entry() {
    gameboy::Emulator emulator{gameboy::Cartridge{rom()}, gameboy::HardwareModel::dmg};
    auto& bus = emulator.bus();
    bus.write8(0xFF40, 0);
    bus.write8(0xFF0F, 0);
    bus.write8(0xFFFF, 6);
    (void)emulator.step();
    (void)emulator.step();
    bus.write8(0xFF04, 0);
    bus.write8(0xFF07, 5);
    bus.write8(0xFF0F, 6);
    check(emulator.step() == 20 && emulator.cpu().registers().pc == 0x48 &&
              bus.debug_divider_counter() == 16 && bus.read8(0xFF05) == 1 &&
              bus.read8(0xFF0F) == 0xE4,
          "mixed STAT/timer entry retains existing timer-vector clocking and the pending timer flag");
    (void)emulator.step();
    check(bus.debug_divider_counter() == 20 && bus.read8(0xFF0F) == 0xE4,
          "STAT acknowledgment does not consume timer requests or alter the vector fetch clock");
}
} // namespace

int main() {
    test_ack_boundary();
    test_save_and_reset();
    test_other_interrupts_and_models();
    test_mixed_stat_timer_entry();
    return failures ? 1 : 0;
}
