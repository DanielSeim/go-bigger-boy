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
    // Version 42 appends flags and three u64 startup-presentation fields
    // after the version-41 acknowledgment boolean. Mutate that boolean,
    // not the last byte of the presentation audio cursor.
    constexpr std::size_t presentation_bytes = 1 + 3 * sizeof(std::uint64_t);
    decoded.payload[decoded.payload.size() - presentation_bytes - 1] = 2;
    const auto malformed = gameboy::save_state_container::encode(
        emulator.rom_fingerprint(), decoded.payload);
    emulator.load_state(pending);
    bool rejected{};
    try { emulator.load_state(malformed); }
    catch (const gameboy::SaveStateError&) { rejected = true; }
    check(rejected && emulator.save_state() == pending,
          "malformed acknowledgment phase is rejected and rolls back atomically");

    decoded = gameboy::save_state_container::decode(pending, emulator.rom_fingerprint());
    decoded.payload.resize(decoded.payload.size() - presentation_bytes);
    auto version41 = gameboy::save_state_container::encode(
        emulator.rom_fingerprint(), decoded.payload);
    version41[8] = 41;
    emulator.load_state(version41);
    (void)emulator.step();
    check(emulator.save_state() == completed,
          "version-41 state retains its acknowledgment phase without presentation fields");

    decoded.payload.pop_back(); // Version 40 predates the acknowledgment boolean too.
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

void test_first_visible_mode_edges() {
    constexpr unsigned frame_start = 154U * 456U - 4U;
    for (auto model : {gameboy::HardwareModel::dmg, gameboy::HardwareModel::mgb,
                       gameboy::HardwareModel::cgb_c, gameboy::HardwareModel::sgb,
                       gameboy::HardwareModel::sgb2}) {
        const bool dmg = model == gameboy::HardwareModel::dmg ||
                         model == gameboy::HardwareModel::mgb;
        for (bool batched : {false, true}) {
            for (unsigned line : {0U, 1U}) {
                for (unsigned source : {0x08U, 0x20U}) {
                    const unsigned edge = frame_start + line * 456U +
                        (source == 0x08 ? 252U : 0U);
                    // Independently authored LD C,0F; LD A,(C) or LDH A,(0F).
                    // Sweep individual T-cycles around the edge, not just M-cycles.
                    for (bool immediate : {false, true}) {
                        for (int offset = -1; offset <= 1; ++offset) {
                            auto bytes = rom();
                            bytes[0x100] = 0x0E;
                            bytes[0x101] = 0x0F;
                            bytes[0x102] = immediate ? 0xF0 : 0xF2;
                            bytes[0x103] = 0x0F;
                            gameboy::Emulator emulator{gameboy::Cartridge{bytes}, model};
                            auto& bus = emulator.bus();
                            bus.debug_set_peripheral_batch_enabled(batched);
                            bus.write8(0xFF40, 0);
                            bus.write8(0xFFFF, 0);
                            check(emulator.step() == 8, "synthetic IF reader sets C normally");
                            bus.write8(0xFF45, 200); // Exclude coincidence as an IRQ source.
                            bus.write8(0xFF41, static_cast<std::uint8_t>(source));
                            bus.write8(0xFF40, 0x80);
                            const unsigned read_cycles = immediate ? 12U : 8U;
                            bus.tick(edge - read_cycles + offset);
                            bus.write8(0xFF0F, 0x15); // Other pending IF bits must survive.
                            const auto saved = emulator.save_state();
                            const bool old_flag = offset < 0 || (dmg && line == 0 && offset == 0);
                            check(emulator.step() == read_cycles &&
                                      emulator.cpu().registers().a == (old_flag ? 0xF5 : 0xF7),
                                  "IF reader samples first-line mode races without delaying later-line flags");
                            check((bus.read8(0xFF0F) & 2U) == (offset < 0 ? 0U : 2U),
                                  "CPU sampling never delays or consumes the actual pending interrupt");
                            const auto completed = emulator.save_state();
                            emulator.load_state(saved);
                            (void)emulator.step();
                            check(emulator.save_state() == completed,
                                  "mode-edge save/load reproduces the exact CPU and PPU state");
                        }
                    }
                    // Save exactly before the internal interrupt edge as well.
                    gameboy::Emulator emulator{gameboy::Cartridge{rom()}, model};
                    auto& bus = emulator.bus();
                    bus.debug_set_peripheral_batch_enabled(batched);
                    bus.write8(0xFF40, 0);
                    bus.write8(0xFF45, 200);
                    bus.write8(0xFF41, static_cast<std::uint8_t>(source));
                    bus.write8(0xFF40, 0x80);
                    bus.tick(edge - 1);
                    bus.write8(0xFF0F, 0);
                    const auto saved = emulator.save_state();
                    bus.tick(1);
                    check((bus.read8(0xFF0F) & 2U) != 0,
                          "mode source rises at the PPU edge without changing CPU sampling");
                    const auto completed = emulator.save_state();
                    emulator.load_state(saved);
                    bus.tick(1);
                    check(emulator.save_state() == completed,
                          "mode-edge sampling phase is represented in existing save-state fields");
                }
            }
        }
    }
}

void test_first_line_arbitration_and_blocking() {
    constexpr unsigned frame_start = 154U * 456U - 4U;
    for (bool batched : {false, true}) {
        gameboy::Emulator emulator{gameboy::Cartridge{rom()}, gameboy::HardwareModel::dmg};
        auto& bus = emulator.bus();
        bus.debug_set_peripheral_batch_enabled(batched);
        bus.write8(0xFF40, 0);
        bus.write8(0xFE00, 0x52);
        bus.write8(0xFF45, 200);
        bus.write8(0xFF41, 0x20);
        bus.write8(0xFF40, 0x80);
        bus.tick(frame_start - 1);
        bus.write8(0xFF0F, 0);
        bus.tick(1);
        check((bus.read8(0xFF41) & 3U) == 0 && bus.read8(0xFE00) == 0xFF,
              "first-line OAM reads lock before visible mode 2");
        bus.write8(0xFE00, 0x91);
        check(bus.debug_read_oam(0) == 0x91 && (bus.read8(0xFF0F) & 2U),
              "IF sampling preserves dot-zero interrupt arbitration and existing OAM writes");
        bus.tick(1);
        check((bus.read8(0xFF41) & 3U) == 2 && (bus.read8(0xFF0F) & 2U),
              "visible OAM mode follows the already-pending interrupt");
        bus.write8(0xFE00, 0x73);
        check(bus.debug_read_oam(0) == 0x91,
              "visible mode 2 retains the existing blocked OAM write behavior");

        // VBlank and OAM selectors share one line, not separate IRQ pulses.
        for (unsigned selection : {0x30U, 0x60U}) {
            gameboy::Emulator combined{gameboy::Cartridge{rom()}, gameboy::HardwareModel::dmg};
            auto& joined = combined.bus();
            joined.debug_set_peripheral_batch_enabled(batched);
            joined.write8(0xFF40, 0);
            joined.write8(0xFF45, selection == 0x60 ? 0 : 200);
            joined.write8(0xFF41, static_cast<std::uint8_t>(selection));
            joined.write8(0xFF40, 0x80);
            joined.tick(frame_start - 1);
            joined.write8(0xFF0F, 0);
            joined.tick(2);
            check(!(joined.read8(0xFF0F) & 2U),
                  "VBlank or coincidence blocks a second STAT edge at the first OAM handoff");
        }
    }
}

void test_first_line_read_preserves_pending_stat() {
    constexpr unsigned frame_start = 154U * 456U - 4U;
    for (bool batched : {false, true}) {
        for (unsigned source : {0x08U, 0x20U}) {
            for (bool pending : {false, true}) {
                auto bytes = rom();
                // LD C,0F; EI; NOP; LD A,(C); NOP.
                bytes[0x100] = 0x0E; bytes[0x101] = 0x0F;
                bytes[0x102] = 0xFB; bytes[0x103] = 0x00;
                bytes[0x104] = 0xF2;
                gameboy::Emulator emulator{gameboy::Cartridge{bytes}, gameboy::HardwareModel::dmg};
                auto& bus = emulator.bus();
                bus.debug_set_peripheral_batch_enabled(batched);
                bus.write8(0xFF40, 0);
                bus.write8(0xFFFF, 0);
                (void)emulator.step(); (void)emulator.step(); (void)emulator.step();
                bus.write8(0xFF45, 200);
                bus.write8(0xFF41, static_cast<std::uint8_t>(source));
                bus.write8(0xFF40, 0x80);
                const unsigned edge = frame_start + (source == 0x08 ? 252U : 0U);
                bus.tick(edge - 8);
                bus.write8(0xFF0F, pending ? 0x17 : 0x15);
                check(emulator.step() == 8 && emulator.cpu().registers().a == (pending ? 0xF7 : 0xF5),
                      "first-line read hides only a new edge, never an already-pending STAT flag");
                bus.write8(0xFFFF, 2);
                check(emulator.step() == 20 && emulator.cpu().registers().pc == 0x48,
                      "a flag hidden from the coincident IF read still dispatches at the next instruction");
            }
        }
    }
}
} // namespace

int main() {
    test_ack_boundary();
    test_save_and_reset();
    test_other_interrupts_and_models();
    test_mixed_stat_timer_entry();
    test_first_visible_mode_edges();
    test_first_line_arbitration_and_blocking();
    test_first_line_read_preserves_pending_stat();
    return failures ? 1 : 0;
}
