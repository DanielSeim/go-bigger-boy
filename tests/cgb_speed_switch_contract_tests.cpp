#include "gameboy/emulator.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::vector<std::uint8_t> rom() {
    std::vector<std::uint8_t> bytes(32768);
    bytes[0x143] = 0x80;
    bytes[0x100] = 0x10; // STOP 0; INC B; NOP
    bytes[0x102] = 4;
    return bytes;
}
void drain(gameboy::Emulator& emulator) {
    unsigned steps = 0;
    while (emulator.cpu().debug_speed_switch_cycles_remaining()) {
        require(emulator.step() == 4, "pause steps remain bounded");
        require(++steps <= 32771, "pause terminates within its clock budget");
    }
}
void count_fetch(void* context, std::uint16_t, std::uint8_t) noexcept {
    ++*static_cast<unsigned*>(context);
}
void test_transition(bool initial_double) {
    gameboy::Emulator emulator{gameboy::Cartridge{rom()}, gameboy::HardwareModel::cgb_e};
    auto& bus = emulator.bus();
    bus.write8(0xFF40, 0);
    bus.write8(0xFFFF, 0);
    if (initial_double) {
        bus.write8(0xFF4D, 1);
        static_cast<void>(emulator.step());
        drain(emulator);
        require(emulator.step() == 8, "initial switch resumes once");
    }
    auto registers = emulator.cpu().registers();
    registers.pc = 0x100;
    registers.b = 0x10;
    emulator.set_cpu_registers(registers);
    bus.write8(0xFF07, 0);
    bus.write8(0xFF04, 0);
    bus.write8(0xFF05, 0x30);
    bus.write8(0xFF07, 4); // 1024 CPU clocks per increment, no overflow.
    bus.write8(0xFF4D, 1);
    unsigned fetches = 0;
    emulator.debug_set_cpu_instruction_observer(count_fetch, &fetches);
    const auto before = bus.debug_normalized_clocks();
    require(emulator.step() == 4, "STOP consumes its opcode cycle at the old speed");
    require(bus.debug_normalized_clocks() - before == (initial_double ? 4 : 8), "STOP clock uses old speed");
    require(bus.double_speed() != initial_double && (bus.read8(0xFF4D) & 1) == 0,
            "switch clears prepare and selects new speed");
    require(!emulator.cpu().stopped(), "speed transition differs from ordinary STOP");
    const auto paused_clock = bus.debug_normalized_clocks();
    const auto paused_cycles = emulator.cpu().total_cycles();
    const auto b = emulator.cpu().registers().b;
    std::vector<std::uint8_t> expected_after_resume;
    // Interrupt requests accumulate but cannot dispatch in the settling phase.
    bus.write8(0xFFFF, 4);
    bus.write8(0xFF0F, 4);
    const unsigned pause_cycles = initial_double ? 131072 : 131084;
    for (unsigned step = 0; step < pause_cycles / 4; ++step) {
        require(emulator.step() == 4, "transition yields every machine cycle");
        require(emulator.cpu().registers().pc == 0x102 && emulator.cpu().registers().b == b,
                "settling never fetches or executes a resumed instruction");
        require(fetches == 1, "observer excludes transition idle steps");
        if (step == 0 || step == 16383 || emulator.cpu().debug_speed_switch_cycles_remaining() <= 12) {
            const auto saved = emulator.save_state();
            gameboy::Emulator restored{gameboy::Cartridge{rom()}, gameboy::HardwareModel::cgb_e};
            restored.load_state(saved);
            require(restored.save_state() == saved, "transition and normalized clock round trip exactly");
            drain(restored);
            require(restored.step() == 8, "restored first instruction includes resumption cycle");
            const auto after = restored.save_state();
            require(expected_after_resume.empty() || after == expected_after_resume,
                    "all pause checkpoints converge to the same resumed machine");
            expected_after_resume = after;
        }
    }
    require(emulator.cpu().total_cycles() - paused_cycles == pause_cycles, "settling clock count is bounded and complete");
    require(bus.debug_normalized_clocks() - paused_clock == (initial_double ? 262144 : 131084),
            "normalized pause duration uses actual new speed");
    require(bus.read8(0xFF05) == 0xB0, "slow TIMA advances by the reference's 128 increments");
    require((bus.read8(0xFF0F) & 4) != 0, "pending interrupt survives settling");
    require(emulator.step() == 8 && emulator.cpu().registers().b == 0x11 && fetches == 2,
            "first resumed instruction incurs one extra machine cycle and executes once");
    require(emulator.save_state() == expected_after_resume, "restoration agrees with uninterrupted continuation");
    require(emulator.step() == 4 && fetches == 3, "later instruction cadence is nominal");
    registers.pc = 0x100;
    emulator.set_cpu_registers(registers);
    bus.write8(0xFF4D, 1);
    static_cast<void>(emulator.step());
    emulator.reset();
    require(emulator.cpu().debug_speed_switch_cycles_remaining() == 0 &&
            !emulator.cpu().debug_speed_switch_resume_pending(), "reset clears an in-flight transition");
}
void test_frames_during_pause() {
    gameboy::Emulator emulator{gameboy::Cartridge{rom()}, gameboy::HardwareModel::cgb_e};
    emulator.bus().write8(0xFF4D, 1);
    static_cast<void>(emulator.step());
    drain(emulator); // Double-speed settling is just shorter than one full frame.
    static_cast<void>(emulator.step());
    auto registers = emulator.cpu().registers();
    registers.pc = 0x100;
    emulator.set_cpu_registers(registers);
    emulator.bus().write8(0xFF4D, 1);
    static_cast<void>(emulator.step());
    unsigned frames = 0;
    while (emulator.cpu().debug_speed_switch_cycles_remaining()) {
        static_cast<void>(emulator.step());
        if (emulator.frame_ready()) { ++frames; emulator.consume_frame(); }
    }
    require(frames >= 1, "PPU frames remain observable while CPU fetch is paused");
}
void test_interrupt_after_pause() {
    auto image = rom();
    image[0x100] = 0xFB; // EI; NOP; STOP 0; INC B
    image[0x101] = 0;
    image[0x102] = 0x10;
    image[0x104] = 4;
    gameboy::Emulator emulator{gameboy::Cartridge{image}, gameboy::HardwareModel::cgb_e};
    auto& bus = emulator.bus();
    bus.write8(0xFF40, 0);
    bus.write8(0xFFFF, 0);
    bus.write8(0xFF0F, 0);
    bus.write8(0xFF4D, 1);
    static_cast<void>(emulator.step());
    static_cast<void>(emulator.step());
    static_cast<void>(emulator.step());
    require(emulator.cpu().interrupts_enabled(), "IME remains enabled across STOP");
    bus.write8(0xFFFF, 4);
    bus.write8(0xFF0F, 4);
    drain(emulator);
    require(emulator.cpu().registers().pc == 0x104, "enabled IRQ cannot dispatch during pause");
    require(emulator.step() == 24 && emulator.cpu().registers().pc == 0x50,
            "pending IRQ dispatch consumes resumption cycle before vector entry");
    require(bus.read16(emulator.cpu().registers().sp) == 0x104,
            "interrupt return address identifies unexecuted resumed instruction");
    require(emulator.step() == 4, "ISR instruction has ordinary cadence");
}
void repair_container(std::vector<std::uint8_t>& state, unsigned version) {
    const auto u32 = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) state[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    u32(8, version);
    u32(20, static_cast<std::uint32_t>(state.size() - 28));
    std::uint32_t crc = 0xFFFFFFFF;
    for (std::size_t i = 28; i < state.size(); ++i) {
        crc ^= state[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    u32(24, ~crc);
}
void test_legacy_and_invalid_states() {
    gameboy::Emulator emulator{gameboy::Cartridge{rom()}, gameboy::HardwareModel::cgb_e};
    emulator.bus().write8(0xFF4D, 1);
    static_cast<void>(emulator.step());
    const auto saved = emulator.save_state();
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        auto bad = saved;
        const auto tail = bad.size() - 13;
        if (mutation == 0) bad[tail] = 1; // Non-machine-cycle countdown.
        if (mutation == 1) bad[tail + 2] = 3; // Out of bounds.
        if (mutation == 2) bad[tail + 4] = 0; // Countdown without resumption.
        repair_container(bad, 44);
        bool rejected = false;
        try { emulator.load_state(bad); } catch (const gameboy::SaveStateError&) { rejected = true; }
        require(rejected && emulator.save_state() == saved, "invalid transition rejects transactionally");
    }
    auto old = saved;
    old.resize(old.size() - 13);
    repair_container(old, 43);
    emulator.load_state(old);
    require(!emulator.cpu().debug_speed_switch_cycles_remaining() &&
            !emulator.cpu().debug_speed_switch_resume_pending() &&
            emulator.bus().debug_normalized_clocks() == 0,
            "version 43 migration clears transition and starts a new clock epoch");
    require(emulator.step() == 4 && emulator.cpu().registers().pc == 0x103,
            "legacy atomic STOP state resumes without inventing a pause");
}
} // namespace
int main() {
    try {
        test_transition(false);
        test_transition(true);
        test_frames_during_pause();
        test_interrupt_after_pause();
        test_legacy_and_invalid_states();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
