#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_spc700.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

int failures{};

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_resumable_bus() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xBA; ipl[1] = 0xF4; // MOVW YA,$F4: separate low/high reads.
    ipl[2] = 0xDA; ipl[3] = 0xF6; // MOVW $F6,YA: separate port writes.
    bus.install_ipl(ipl);
    bus.host_write_port(0, 0x17);
    bus.host_write_port(1, 0x22);
    gameboy::SnesSpc700 cpu(bus);
    check(!cpu.clock().instruction.supported && cpu.cycles() == 0,
          "continuation requires explicit cycle-bus opt-in");
    cpu.set_cycle_bus_enabled(true);
    for (unsigned clock = 1; clock <= 3; ++clock) {
        const auto result = cpu.clock();
        check(result.instruction.supported && !result.completed && cpu.cycles() == clock,
              "continuation advances exactly one clock and suspends the word read");
    }
    check(cpu.registers().pc == 0xFFC0 && cpu.instruction_pending(),
          "suspended instruction exposes only committed architectural state");
    check(!cpu.step().supported && cpu.cycles() == 3,
          "instruction stepping cannot skip a suspended bus access");
    cpu.set_cycle_bus_enabled(false); // In-flight mode changes are ignored.
    bus.host_write_port(0, 0x99);
    bus.host_write_port(1, 0x33);
    check(!cpu.clock().completed && cpu.cycles() == 4,
          "the word-read internal idle is independently resumable");
    const auto read = cpu.clock();
    check(read.completed && read.instruction.supported && read.instruction.cycles == 5 &&
              cpu.registers().a == 0x17 && cpu.registers().y == 0x33 &&
              !cpu.instruction_pending(),
          "past read is latched while future read sees an intervening host write");
    for (unsigned clock = 1; clock <= 4; ++clock) {
        const auto result = cpu.clock();
        check(!result.completed && cpu.cycles() == 5 + clock,
              "word store suspends up to and between its two writes");
    }
    check(bus.host_read_port(2) == 0x17 && bus.host_read_port(3) == 0,
          "only the first word-store port has been written at clock four");
    const auto write = cpu.clock();
    check(write.completed && write.instruction.cycles == 5 &&
              bus.host_read_port(3) == 0x33 && cpu.cycles() == 10,
          "second word-store port commits only on its own clock");
    (void)cpu.clock();
    cpu.reset();
    check(!cpu.instruction_pending() && cpu.cycles() == 0 &&
              cpu.clock().instruction.supported && cpu.cycles() == 1,
          "reset discards an unfinished continuation without replaying its effects");
    cpu.reset();
    ipl[0] = 0xFF;
    bus.install_ipl(ipl);
    const auto unsupported = cpu.clock();
    check(unsupported.completed && !unsupported.instruction.supported &&
              cpu.cycles() == 1 && cpu.registers().pc == 0xFFC0,
          "unsupported opcode fails closed after its single actual fetch clock");
}

void test_write_cycle_observation() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x5A; // MOV A,#$5A
    ipl[2] = 0xC4; ipl[3] = 0x20; // MOV $20,A
    ipl[4] = 0xFF; // Explicitly unsupported.
    bus.install_ipl(ipl);
    bus.dsp_write_ram(0x20, 0x17);
    gameboy::SnesSpc700 cpu(bus);
    struct Observation {
        gameboy::SnesApuBus* bus;
        unsigned count{};
        std::array<std::uint64_t, 8> cycles{};
        std::array<std::uint8_t, 8> memory{};
        std::array<bool, 8> after{};
    } observation{&bus};
    cpu.set_write_cycle_observer(
        [](void* context, std::uint64_t cycle, std::uint8_t,
           std::uint16_t address, std::uint8_t, bool after) noexcept {
            auto& state = *static_cast<Observation*>(context);
            if (state.count < state.cycles.size()) {
                state.cycles[state.count] = cycle;
                state.memory[state.count] = state.bus->dsp_read_ram(address);
                state.after[state.count] = after;
            }
            ++state.count;
        }, &observation);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 4,
          "write observation preserves instruction cycle counts");
    check(observation.count == 2 && observation.cycles[0] == 6 &&
              observation.cycles[1] == 6 && !observation.after[0] &&
              observation.after[1] && observation.memory[0] == 0x17 &&
              observation.memory[1] == 0x5A,
          "write callbacks bracket the physical bus mutation at its cycle");
    check(!cpu.step().supported && cpu.cycles() == 6,
          "unsupported instructions do not advance observed clocks");
    cpu.reset();
    check(cpu.cycles() == 0 && cpu.registers().pc == 0xFFC0,
          "reset clears observed clocks and restores IPL entry");
    check(cpu.step().supported && cpu.step().supported && observation.count == 4 &&
              observation.cycles[2] == 6,
          "reset preserves the installed observer with a fresh clock origin");
    cpu.set_write_cycle_observer(nullptr);
    cpu.reset();
    check(cpu.step().supported && cpu.step().supported && observation.count == 4,
          "detaching write observation stops callbacks");

    ipl = {};
    ipl[0] = 0xE8; ipl[1] = 0x4C; // MOV A,#KON
    ipl[2] = 0x8D; ipl[3] = 0x04; // MOV Y,#voice2
    ipl[4] = 0xDA; ipl[5] = 0xF2; // MOVW $F2,YA
    bus.install_ipl(ipl);
    cpu.reset();
    observation.count = 0;
    cpu.set_write_cycle_observer(
        [](void* context, std::uint64_t cycle, std::uint8_t,
           std::uint16_t, std::uint8_t, bool) noexcept {
            auto& state = *static_cast<Observation*>(context);
            if (state.count < state.cycles.size()) state.cycles[state.count] = cycle;
            ++state.count;
        }, &observation);
    check(cpu.step().supported && cpu.step().supported && cpu.step().cycles == 5 &&
              cpu.cycles() == 9 && observation.count == 4 &&
              observation.cycles[0] == 8 && observation.cycles[1] == 8 &&
              observation.cycles[2] == 9 && observation.cycles[3] == 9 &&
              bus.dsp_register(0x4C) == 4,
          "MOVW observes separate DSP address and data writes in bus order");
}

void test_cycle_bus_timers() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE4; ipl[1] = 0xFD; // MOV A,timer0
    bus.install_ipl(ipl);
    bus.spc_write(0xFA, 1);
    bus.spc_write(0xF1, 0x81);
    bus.tick(126);
    gameboy::SnesSpc700 cpu(bus);
    cpu.set_cycle_bus_enabled(true);
    check(cpu.step().cycles == 3 && cpu.registers().a == 1 &&
              bus.spc_read(0xFD) == 0,
          "cycle-level timer read observes rollover before access and clears output");

    bus.reset();
    ipl = {};
    ipl[0] = 0xC4; ipl[1] = 0xFD; // MOV timer0,A includes a destructive dummy read.
    bus.install_ipl(ipl);
    bus.spc_write(0xFA, 1);
    bus.spc_write(0xF1, 0x81);
    bus.tick(125);
    cpu.reset();
    check(cpu.step().cycles == 4 && bus.spc_read(0xFD) == 0,
          "store destination dummy read clears timer output at its own cycle");

    bus.reset();
    ipl = {};
    ipl[0] = 0x8F; ipl[1] = 0x84; ipl[2] = 0xF1; // Enable timer2 at cycle5.
    bus.install_ipl(ipl);
    bus.spc_write(0xFC, 1);
    bus.tick(11);
    cpu.reset();
    check(cpu.step().cycles == 5 && bus.spc_read(0xFF) == 0,
          "timer enable write does not retroactively count earlier instruction clocks");
    bus.tick(16);
    check(bus.spc_read(0xFF) == 1,
          "cycle-level timer enable preserves the free-running divider phase");

    // Default execution remains the unchanged instruction-granular baseline.
    bus.reset();
    ipl = {};
    ipl[0] = 0xE4; ipl[1] = 0xFD;
    bus.install_ipl(ipl);
    bus.spc_write(0xFA, 1);
    bus.spc_write(0xF1, 0x81);
    bus.tick(126);
    cpu.reset();
    cpu.set_cycle_bus_enabled(false);
    check(cpu.step().cycles == 3 && cpu.registers().a == 0 && bus.spc_read(0xFD) == 1,
          "legacy timer read and instruction-tail clocking remain available");
}

void test_synthetic_upload() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    // Independent synthetic handshake: advertise readiness, wait for CC,
    // then copy one host-supplied byte to a host-supplied APU-RAM address.
    // This is test code, not the SNES IPL ROM or any SGB firmware bytes.
    constexpr std::array<std::uint8_t, 35> program{
        0x8F, 0xAA, 0xF4, // MOV $F4,#$AA
        0x8F, 0xBB, 0xF5, // MOV $F5,#$BB
        0x78, 0xCC, 0xF4, // CMP $F4,#$CC
        0xD0, 0xFB,       // BNE wait
        0x8F, 0xCC, 0xF4, // MOV $F4,#$CC
        0xEB, 0xF6,       // MOV Y,$F6 (destination low)
        0xCB, 0x00,       // MOV $00,Y
        0xEB, 0xF7,       // MOV Y,$F7 (destination high)
        0xCB, 0x01,       // MOV $01,Y
        0x8F, 0x00, 0x02, // MOV $02,#$00
        0xEB, 0x02,       // MOV Y,$02
        0xE4, 0xF5,       // MOV A,$F5 (data)
        0xD7, 0x00,       // MOV [$00]+Y,A
        0xCB, 0xF4,       // MOV $F4,Y (acknowledge index 0)
        0x2F, 0xFE,       // BRA forever
    };
    static_assert(program.size() <= 64);
    for (std::size_t index = 0; index < program.size(); ++index) {
        ipl[index] = program[index];
    }
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.registers().pc == 0xFFC0, "SPC700 reset fetches IPL overlay");
    check(cpu.step().cycles == 5 && cpu.step().cycles == 5 &&
              bus.host_read_port(0) == 0xAA && bus.host_read_port(1) == 0xBB,
          "SPC700 program advertises boot readiness on independent host ports");
    check(cpu.step().supported && cpu.step().supported &&
              cpu.registers().pc == 0xFFC6,
          "SPC700 waits with a backward conditional branch");

    bus.host_write_port(0, 0xCC);
    bus.host_write_port(1, 0x4D);
    bus.host_write_port(2, 0x00);
    bus.host_write_port(3, 0x40);
    for (unsigned step = 0; step < 20 && bus.host_read_port(0) != 0; ++step) {
        const auto result = cpu.step();
        check(result.supported, "synthetic boot upload uses supported instructions");
        if (!result.supported) break;
    }
    check(bus.spc_read(0x4000) == 0x4D && bus.host_read_port(0) == 0,
          "host byte reaches APU RAM through actual SPC700 instruction execution");
    const auto loop_pc = cpu.registers().pc;
    check(cpu.step().supported && cpu.registers().pc == loop_pc,
          "SPC700 BRA uses signed relative displacement");
}

void test_upload_addressing_and_trap() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    // MOV X,#0; DEC X; MOV A,#$5A; MOV (X),A; MOVW YA,$FE;
    // MOVW $20,YA; MOV A,Y; MOV X,A; JMP [$20+X].
    constexpr std::array<std::uint8_t, 19> program{
        0xCD, 0x00, 0x1D, 0xE8, 0x5A, 0xC6,
        0xBA, 0xFE, 0xDA, 0x20, 0xDD, 0x5D,
        0x1F, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    bus.spc_write(0xFE, 0x34); // I/O counter overlay reads zero, not physical RAM.
    bus.spc_write(0xFF, 0x12);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().supported &&
              cpu.registers().x == 0xFF &&
              (cpu.registers().psw & 0x80U) != 0,
          "DEC X wraps and sets negative");
    check(cpu.step().supported && cpu.step().supported &&
              bus.spc_read(0x00FF) == 0 && cpu.registers().a == 0x5A,
          "MOV (X),A writes physical timer-overlay RAM while reads see timer output");
    check(cpu.step().supported && cpu.step().supported &&
              cpu.registers().a == 0 && cpu.registers().y == 0 &&
              bus.spc_read(0x20) == 0 && bus.spc_read(0x21) == 0,
          "MOVW reads through I/O overlays and stores low then high bytes");
    check(cpu.step().supported && cpu.step().supported &&
              cpu.registers().x == 0,
          "MOV A,Y and MOV X,A update register state");
    check(cpu.step().supported && cpu.registers().pc == 0,
          "indirect absolute indexed jump reads a 16-bit RAM pointer");
    const auto result = cpu.step();
    check(result.supported && result.opcode == 0 && result.cycles == 2,
          "RAM opcode at jump target executes as NOP");

    bus.reset();
    ipl.fill(0);
    ipl[0] = 0xFF; // unsupported STOP, never silently executed as a NOP
    bus.install_ipl(ipl);
    cpu.reset();
    const auto unsupported = cpu.step();
    check(!unsupported.supported && unsupported.opcode == 0xFF &&
              unsupported.cycles == 0 && cpu.registers().pc == 0xFFC0,
          "unsupported opcodes trap without advancing execution or clocks");
}

void test_boot_arithmetic_and_branch_flags() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 19> program{
        0x8F, 0x01, 0x30, // MOV $30,#1
        0xAB, 0x30,       // INC $30
        0xEB, 0x30,       // MOV Y,$30
        0x7E, 0x30,       // CMP Y,$30
        0x10, 0x02,       // BPL +2 (skip the next instruction)
        0xE8, 0xFF,       // MOV A,#$FF
        0xFC,             // INC Y
        0xE8, 0x42,       // MOV A,#$42
        0xC4, 0x31,       // MOV $31,A
        0x00,             // NOP
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().supported &&
              bus.spc_read(0x30) == 2,
          "direct-page increment stores the new value");
    check(cpu.step().supported && cpu.step().supported &&
              (cpu.registers().psw & 0x03U) == 0x03U,
          "equal compare sets zero and carry without setting negative");
    check(cpu.step().cycles == 4 && cpu.registers().pc == 0xFFCD,
          "BPL takes the signed branch after a non-negative comparison");
    check(cpu.step().supported && cpu.step().supported &&
              cpu.step().supported && bus.spc_read(0x31) == 0x42 &&
              cpu.registers().y == 3,
          "INC Y and MOV direct,A continue after the skipped instruction");
    check(cpu.step().supported && cpu.registers().a == 0x42,
          "NOP leaves registers unchanged");
}

void test_word_transfer() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xBA; ipl[1] = 0x40; // MOVW YA,$40
    ipl[2] = 0xDA; ipl[3] = 0x50; // MOVW $50,YA
    bus.install_ipl(ipl);
    bus.spc_write(0x40, 0x34);
    bus.spc_write(0x41, 0x92);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 5 && cpu.registers().a == 0x34 &&
              cpu.registers().y == 0x92 &&
              (cpu.registers().psw & 0x80U) != 0,
          "MOVW reads a little-endian word and sets N from its high byte");
    check(cpu.step().cycles == 5 && bus.spc_read(0x50) == 0x34 &&
              bus.spc_read(0x51) == 0x92,
          "MOVW writes both bytes in low-then-high order");
}

void test_direct_page_selection() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 7> program{
        0x40,             // SETP
        0x8F, 0x42, 0x30, // MOV $30,#$42 (page one)
        0x20,             // CLRP
        0xE4, 0x30,       // MOV A,$30 (page zero)
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    bus.spc_write(0x30, 0x19);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && (cpu.registers().psw & 0x20U) != 0,
          "SETP selects direct page one in two cycles");
    check(cpu.step().supported && bus.spc_read(0x130) == 0x42 &&
              bus.spc_read(0x30) == 0x19,
          "direct-page store uses the selected page");
    check(cpu.step().cycles == 2 && (cpu.registers().psw & 0x20U) == 0,
          "CLRP restores direct page zero in two cycles");
    check(cpu.step().supported && cpu.registers().a == 0x19,
          "direct-page load uses page zero after CLRP");
}

void test_indexed_post_increment_store() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 7> program{
        0xCD, 0xFF, // MOV X,#$FF
        0xE8, 0x3A, // MOV A,#$3A
        0xAF,       // MOV (X)+,A
        0xAF,       // MOV (X)+,A: wrap to zero
        0x00,
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().supported,
          "indexed post-increment setup executes");
    check(cpu.step().cycles == 4 && cpu.registers().x == 0 &&
              bus.dsp_read_ram(0xFF) == 0x3A,
          "MOV (X)+,A stores then wraps the index");
    check(cpu.step().cycles == 4 && cpu.registers().x == 1 &&
              bus.spc_read(0) == 0x3A,
          "post-increment store uses wrapped direct-page address");
}

void test_compare_x_immediate() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 8> program{
        0xCD, 0x7F, // MOV X,#$7F
        0xC8, 0x7F, // equal
        0xC8, 0x80, // below
        0xC8, 0x01, // above
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported, "CMP X setup executes");
    check(cpu.step().cycles == 2 && (cpu.registers().psw & 0x83U) == 0x03U,
          "CMP X immediate sets zero and carry on equality");
    check(cpu.step().cycles == 2 && (cpu.registers().psw & 0x83U) == 0x80U,
          "CMP X immediate sets negative and clears carry below operand");
    check(cpu.step().cycles == 2 && (cpu.registers().psw & 0x83U) == 0x01U &&
              cpu.registers().x == 0x7F,
          "CMP X immediate preserves X and sets carry above operand");
}

void test_increment_x_wrap() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0xFF; // MOV X,#$FF
    ipl[2] = 0x3D;                // INC X
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported, "INC X setup executes");
    check(cpu.step().cycles == 2 && cpu.registers().x == 0 &&
              (cpu.registers().psw & 0x82U) == 0x02U,
          "INC X wraps to zero and updates N/Z in two cycles");
}

void test_increment_a_wrap() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0xFF; // MOV A,#$FF
    ipl[2] = 0xBC;                // INC A
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported, "INC A setup executes");
    check(cpu.step().cycles == 2 && cpu.registers().a == 0 &&
              (cpu.registers().psw & 0x82U) == 0x02U,
          "INC A wraps to zero and updates N/Z in two cycles");
}

void test_absolute_indexed_store() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 8> program{
        0xCD, 0x03,       // MOV X,#3
        0xE8, 0x5A,       // MOV A,#$5A
        0xD5, 0xFE, 0x3F, // MOV $3FFE+X,A, crossing page boundary
        0x00,
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().supported,
          "absolute indexed store setup executes");
    check(cpu.step().cycles == 6 && bus.dsp_read_ram(0x4001) == 0x5A &&
              cpu.registers().x == 3,
          "MOV absolute+X,A crosses page boundary without changing X");
}

void test_indirect_indexed_store() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 8> program{
        0xCD, 0x02,       // MOV X,#2
        0xE8, 0x5A,       // MOV A,#$5A
        0xC7, 0xFE,       // MOV [$FE+X],A, pointer wraps to $00
        0x00, 0x00,
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    bus.spc_write(0, 0x01);
    bus.spc_write(1, 0x40);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().supported,
          "indirect indexed store setup executes");
    check(cpu.step().cycles == 7 && bus.dsp_read_ram(0x4001) == 0x5A,
          "MOV [dp+X],A wraps the direct-page pointer before dereference");
}

void test_call_return_stack() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 10> program{
        0x3F, 0xC7, 0xFF, // CALL $FFC7
        0xE8, 0x5A,       // MOV A,#$5A after return
        0x2F, 0xFE,       // BRA self
        0xE8, 0x12,       // MOV A,#$12 in subroutine
        0x6F,             // RET
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 8 && cpu.registers().pc == 0xFFC7 &&
              cpu.registers().sp == 0xFE &&
              bus.dsp_read_ram(0x100) == 0xFF &&
              bus.dsp_read_ram(0x1FF) == 0xC3,
          "CALL pushes next PC high then low and jumps in eight cycles");
    check(cpu.step().supported && cpu.registers().a == 0x12,
          "subroutine body executes");
    check(cpu.step().cycles == 5 && cpu.registers().pc == 0xFFC3 &&
              cpu.registers().sp == 0,
          "RET pops exact next PC and restores stack pointer");
    check(cpu.step().supported && cpu.registers().a == 0x5A,
          "execution resumes after CALL without an extra increment");
}

void test_load_y_immediate() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 0x80;
    ipl[2] = 0x8D; ipl[3] = 0;
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.registers().y == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80U,
          "MOV Y immediate sets negative for bit seven");
    check(cpu.step().cycles == 2 && cpu.registers().y == 0 &&
              (cpu.registers().psw & 0x82U) == 0x02U,
          "MOV Y immediate sets zero for zero operand");
}

void test_store_y_absolute() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 5> program{
        0x8D, 0x73,       // MOV Y,#$73
        0xCC, 0x01, 0x40, // MOV $4001,Y
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported, "absolute Y store setup executes");
    check(cpu.step().cycles == 5 && bus.dsp_read_ram(0x4001) == 0x73 &&
              cpu.registers().y == 0x73,
          "MOV absolute,Y stores Y without changing it");
}

void test_store_a_absolute() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 5> program{
        0xE8, 0x73,       // MOV A,#$73
        0xC5, 0x01, 0x40, // MOV $4001,A
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported, "absolute A store setup executes");
    check(cpu.step().cycles == 5 && bus.dsp_read_ram(0x4001) == 0x73 &&
              cpu.registers().a == 0x73,
          "MOV absolute,A stores A without changing it");
}

void test_load_a_absolute() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE5; ipl[1] = 0x02; ipl[2] = 0x40;
    bus.install_ipl(ipl);
    bus.spc_write(0x4002, 0x81);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 4 && cpu.registers().a == 0x81 &&
              (cpu.registers().psw & 0x82U) == 0x80U,
          "MOV A,absolute loads through bus and updates negative");
}

void test_load_a_absolute_y() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 5> program{
        0x8D, 0x03,       // MOV Y,#3
        0xF6, 0xFE, 0x3F, // MOV A,$3FFE+Y
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    bus.spc_write(0x4001, 0x91);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported, "absolute Y load setup executes");
    check(cpu.step().cycles == 5 && cpu.registers().a == 0x91 &&
              (cpu.registers().psw & 0x82U) == 0x80U,
          "MOV A,absolute+Y crosses page boundary and updates flags");
}

void test_compare_a_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 4> program{
        0xE8, 0x81, // MOV A,#$81
        0x64, 0x30, // CMP A,$30
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    bus.spc_write(0x30, 0x82);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported, "CMP A direct setup executes");
    check(cpu.step().cycles == 3 && cpu.registers().a == 0x81 &&
              (cpu.registers().psw & 0x83U) == 0x80U,
          "CMP A,direct compares without changing A");
}

void test_equal_branch() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 8> program{
        0xE8, 0x00, // MOV A,#0: Z=1
        0xF0, 0x02, // BEQ +2
        0xE8, 0xFF, // skipped
        0xF0, 0x00, // BEQ +0
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported, "BEQ setup executes");
    check(cpu.step().cycles == 4 && cpu.registers().pc == 0xFFC6 &&
              cpu.registers().a == 0,
          "BEQ takes branch when zero flag is set");
    check(cpu.step().cycles == 4 && cpu.registers().pc == 0xFFC8,
          "BEQ with zero displacement still incurs taken-branch cycles");
}

void test_and_a_immediate() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 6> program{
        0xE8, 0xF0, // MOV A,#$F0
        0x28, 0x0F, // AND A,#$0F -> zero
        0x28, 0xFF, // AND A,#$FF -> still zero
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported, "AND immediate setup executes");
    check(cpu.step().cycles == 2 && cpu.registers().a == 0 &&
              (cpu.registers().psw & 0x82U) == 0x02U,
          "AND A immediate updates accumulator and zero flag");
    check(cpu.step().cycles == 2 && cpu.registers().a == 0,
          "AND A immediate preserves a zero result");
}

void test_eor_a_immediate() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 6> program{
        0xE8, 0x7F, // MOV A,#$7F
        0x48, 0xFF, // EOR A,#$FF -> $80
        0x48, 0x80, // EOR A,#$80 -> zero
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported, "EOR immediate setup executes");
    check(cpu.step().cycles == 2 && cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80U,
          "EOR A immediate sets negative on bit seven");
    check(cpu.step().cycles == 2 && cpu.registers().a == 0 &&
              (cpu.registers().psw & 0x82U) == 0x02U,
          "EOR A immediate sets zero after cancellation");
}

void test_direct_bit_branches() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    constexpr std::array<std::uint8_t, 12> program{
        0xF3, 0x30, 0x03, // BBC7 $30,+3: taken
        0xE8, 0xFF,       // skipped
        0x00,             // skipped
        0xE3, 0x30, 0x03, // BBS7 $30,+3: not taken
        0x03, 0x31, 0xFD, // BBS0 $31,-3: taken, loops to BBS7
    };
    for (std::size_t index = 0; index < program.size(); ++index) ipl[index] = program[index];
    bus.install_ipl(ipl);
    bus.spc_write(0x30, 0x7F);
    bus.spc_write(0x31, 0x01);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 7 && cpu.registers().pc == 0xFFC6,
          "BBC7 takes branch when direct-page bit seven is clear");
    check(cpu.step().cycles == 5 && cpu.registers().pc == 0xFFC9,
          "BBS7 does not branch when bit seven is clear");
    check(cpu.step().cycles == 7 && cpu.registers().pc == 0xFFC9,
          "BBS0 takes signed backward branch when bit zero is set");
}

void test_decrement_y_branch() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 2;     // MOV Y,#2
    ipl[2] = 0xFE; ipl[3] = 0xFE; // DBNZ Y,-2
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.registers().y == 2,
          "MOV Y initializes DBNZ test");
    const auto flags = cpu.registers().psw;
    check(cpu.step().cycles == 6 && cpu.registers().y == 1 &&
              cpu.registers().pc == 0xFFC2 && cpu.registers().psw == flags,
          "DBNZ Y taken branch costs six clocks and preserves flags");
    check(cpu.step().cycles == 4 && cpu.registers().y == 0 &&
              cpu.registers().pc == 0xFFC4 && cpu.registers().psw == flags,
          "DBNZ Y not-taken branch costs four clocks and preserves flags");
}

void test_or_a_immediate() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x80; // MOV A,#$80
    ipl[2] = 0x08; ipl[3] = 0x01; // OR A,#$01
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 2 &&
              cpu.registers().a == 0x81 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "OR A,#imm combines bits and updates N/Z");
}

void test_asl_a() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x80; // MOV A,#$80
    ipl[2] = 0x1C;                 // ASL A
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 2 &&
              cpu.registers().a == 0 &&
              (cpu.registers().psw & 0x83U) == 0x03,
          "ASL A shifts bit seven to carry and updates N/Z");
}

void test_set_carry() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x80; // SETC
    ipl[1] = 0x60; // CLRC
    ipl[2] = 0xED; // NOTC
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    const auto flags = cpu.registers().psw;
    check(cpu.step().cycles == 2 &&
              cpu.registers().psw == (flags | 0x01U),
          "SETC sets only carry in two SPC cycles");
    check(cpu.step().cycles == 2 && cpu.registers().psw == flags,
          "CLRC clears only carry in two SPC cycles");
    check(cpu.step().cycles == 3 && cpu.registers().psw == (flags | 0x01U),
          "NOTC toggles only carry in three SPC cycles");
}

void test_adc_a_immediate() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x7F; // MOV A,#$7f
    ipl[2] = 0x80;                 // SETC
    ipl[3] = 0x88; ipl[4] = 0x00; // ADC A,#0
    ipl[5] = 0xE8; ipl[6] = 0xFF; // MOV A,#$ff
    ipl[7] = 0x80;                 // SETC
    ipl[8] = 0x88; ipl[9] = 0x00; // ADC A,#0
    ipl[10] = 0xE8; ipl[11] = 1;   // MOV A,#1
    ipl[12] = 0x8F; ipl[13] = 2; ipl[14] = 0x20; // MOV $20,#2
    ipl[15] = 0x60;                // CLRC
    ipl[16] = 0x84; ipl[17] = 0x20; // ADC A,$20
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().supported &&
              cpu.step().cycles == 2 && cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0xCBU) == 0xC8,
          "ADC immediate sets signed overflow, half carry and negative");
    check(cpu.step().supported && cpu.step().supported &&
              cpu.step().cycles == 2 && cpu.registers().a == 0 &&
              (cpu.registers().psw & 0xCBU) == 0x0B,
          "ADC immediate wraps and sets carry, half carry and zero");
    check(cpu.step().supported && cpu.step().supported &&
              cpu.step().supported && cpu.step().cycles == 3 &&
              cpu.registers().a == 3 && (cpu.registers().psw & 0xCBU) == 0,
          "ADC direct page reads source byte and updates arithmetic flags");
}

void test_absolute_jump() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x5F; ipl[1] = 0xC5; ipl[2] = 0xFF; // JMP $FFC5
    ipl[5] = 0x00; // NOP
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 3 && cpu.registers().pc == 0xFFC5 &&
              cpu.step().cycles == 2,
          "JMP absolute changes SPC program counter in three cycles");
}

void test_set_direct_bit() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xA2; ipl[1] = 0x20; // SET1 $20.5
    ipl[2] = 0x02; ipl[3] = 0x20; // SET1 $20.0
    ipl[4] = 0xB2; ipl[5] = 0x20; // CLR1 $20.5
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    const auto flags = cpu.registers().psw;
    check(cpu.step().cycles == 4 && bus.spc_read(0x20) == 0x20 &&
              cpu.registers().psw == flags,
          "SET1 sets selected direct-page bit without changing flags");
    check(cpu.step().cycles == 4 && bus.spc_read(0x20) == 0x21,
          "SET1 preserves other direct-page bits");
    check(cpu.step().cycles == 4 && bus.spc_read(0x20) == 0x01 &&
              cpu.registers().psw == flags,
          "CLR1 clears selected direct-page bit without changing flags");
}

void test_multiply_ya() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x80; // MOV A,#$80
    ipl[2] = 0x8D; ipl[3] = 0x02; // MOV Y,#2
    ipl[4] = 0xCF;                // MUL YA -> $0100
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().supported &&
              cpu.step().cycles == 9 && cpu.registers().a == 0 &&
              cpu.registers().y == 1 && (cpu.registers().psw & 0x82U) == 0,
          "MUL YA writes both result bytes and bases N/Z on Y");
}

void test_load_a_absolute_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 2; // MOV X,#2
    ipl[2] = 0xF5; ipl[3] = 0xFE; ipl[4] = 0x1F; // MOV A,$1ffe+X
    bus.install_ipl(ipl);
    bus.spc_write(0x2000, 0x81);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().cycles == 5 &&
              cpu.registers().a == 0x81 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV A,absolute+X reads indexed address and sets N/Z");
}

void test_move_a_to_y() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x80; // MOV A,#$80
    ipl[2] = 0xFD;                 // MOV Y,A
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().cycles == 2 &&
              cpu.registers().y == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV Y,A copies accumulator and updates N/Z");
}

void test_load_x_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xF8; ipl[1] = 0x30; // MOV X,$30
    bus.install_ipl(ipl);
    bus.spc_write(0x30, 0x80);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 3 && cpu.registers().x == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV X,dp reads physical direct page and updates N/Z");
}

void test_compare_y_immediate() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 1; // MOV Y,#1
    ipl[2] = 0xAD; ipl[3] = 2; // CMP Y,#2
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().cycles == 2 &&
              cpu.registers().y == 1 &&
              (cpu.registers().psw & 0x83U) == 0x80,
          "CMP Y,#imm sets N/Z/C from subtraction without changing Y");
}

void test_compare_a_immediate() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x80; // MOV A,#$80
    ipl[2] = 0x68; ipl[3] = 0x80; // CMP A,#$80
    ipl[4] = 0x68; ipl[5] = 0x81; // CMP A,#$81
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 2 &&
              (cpu.registers().psw & 0x83U) == 0x03 &&
              cpu.step().cycles == 2 && cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0x83U) == 0x80,
          "CMP A,#imm updates comparison flags and leaves A unchanged");
}

void test_branch_carry_set() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xB0; ipl[1] = 2; // BCS +2, initially not taken
    ipl[2] = 0x80;             // SETC
    ipl[3] = 0xB0; ipl[4] = 0xFE; // BCS -2, taken
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.registers().pc == 0xFFC2 &&
              cpu.step().cycles == 2 && cpu.step().cycles == 4 &&
              cpu.registers().pc == 0xFFC3,
          "BCS uses carry, signed displacement and conditional cycle count");
}

void test_branch_carry_clear() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x90; ipl[1] = 2; // BCC +2, initially taken
    ipl[4] = 0x80;             // SETC
    ipl[5] = 0x90; ipl[6] = 0xFE; // BCC -2, not taken
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 4 && cpu.registers().pc == 0xFFC4 &&
              cpu.step().cycles == 2 && cpu.step().cycles == 2 &&
              cpu.registers().pc == 0xFFC7,
          "BCC uses carry and conditional cycle count");
}

void test_pop_y() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 0x80; // MOV Y,#$80
    ipl[2] = 0x6D;                 // PUSH Y
    ipl[3] = 0x8D; ipl[4] = 0;    // MOV Y,#0
    ipl[5] = 0xEE;                 // POP Y
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 4 &&
              bus.spc_read(0x100) == 0x80 && cpu.registers().sp == 0xFF &&
              cpu.step().cycles == 2 && cpu.step().cycles == 4 &&
              cpu.registers().y == 0x80 && cpu.registers().sp == 0 &&
              (cpu.registers().psw & 0x82U) == 0x02,
          "POP Y restores the stack value without changing flags");
}

void test_load_a_direct_indexed_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x30; // MOV X,#$30
    ipl[2] = 0xF4; ipl[3] = 0xF0; // MOV A,$f0+X, wraps to $20
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0x80);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 4 &&
              cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV A,dp+X wraps the direct offset and updates N/Z");
}

void test_shift_direct_left() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x0B; ipl[1] = 0x30; // ASL $30
    bus.install_ipl(ipl);
    bus.spc_write(0x30, 0x80);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 4 && bus.spc_read(0x30) == 0 &&
              (cpu.registers().psw & 0x83U) == 0x03,
          "ASL dp shifts RAM and sets C/Z from the result");
}

void test_test_and_set_absolute() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x0F; // MOV A,#$0f
    ipl[2] = 0x0E; ipl[3] = 0x20; ipl[4] = 0x02; // TSET1 $0220
    bus.install_ipl(ipl);
    bus.spc_write(0x220, 0xF0);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 6 &&
              bus.spc_read(0x220) == 0xFF && cpu.registers().a == 0x0F &&
              (cpu.registers().psw & 0x82U) == 0,
          "TSET1 tests A minus old memory and sets selected bits");
}

void test_or_direct_immediate() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x18; ipl[1] = 0x0F; ipl[2] = 0x30;
    bus.install_ipl(ipl);
    bus.spc_write(0x30, 0x80);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 5 && bus.spc_read(0x30) == 0x8F &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "OR dp,#imm decodes immediate before direct address");
}

void test_decrement_direct_and_branch() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x6E; ipl[1] = 0x30; ipl[2] = 0xFD; // DBNZ $30,-3
    bus.install_ipl(ipl);
    bus.spc_write(0x30, 2);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 7 && bus.spc_read(0x30) == 1 &&
              cpu.registers().pc == 0xFFC0 && cpu.step().cycles == 5 &&
              bus.spc_read(0x30) == 0 && cpu.registers().pc == 0xFFC3 &&
              cpu.registers().psw == 0,
          "DBNZ dp uses signed branch, 5/7 cycles and preserves flags");
}

void test_shift_direct_right() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x4B; ipl[1] = 0x30; // LSR $30
    bus.install_ipl(ipl);
    bus.spc_write(0x30, 1);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 4 && bus.spc_read(0x30) == 0 &&
              (cpu.registers().psw & 0x83U) == 0x03,
          "LSR dp shifts memory and sets carry and zero");
}

void test_load_a_indirect_direct_y() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 2; // MOV Y,#2
    ipl[2] = 0xF7; ipl[3] = 0x30; // MOV A,[$30]+Y
    bus.install_ipl(ipl);
    bus.spc_write(0x30, 0x20);
    bus.spc_write(0x31, 0x02);
    bus.spc_write(0x222, 0x80);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 6 &&
              cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV A,[dp]+Y loads indexed indirect memory and sets N/Z");
}

void test_increment_word_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x3A; ipl[1] = 0x30; // INCW $30
    bus.install_ipl(ipl);
    bus.spc_write(0x30, 0xFF);
    bus.spc_write(0x31, 0x7F);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 6 && bus.spc_read(0x30) == 0 &&
              bus.spc_read(0x31) == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "INCW dp propagates carry to high byte and sets 16-bit N/Z");
}

void test_push_a() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x80; // MOV A,#$80
    ipl[2] = 0x2D; // PUSH A
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 4 &&
              bus.spc_read(0x100) == 0x80 && cpu.registers().sp == 0xFF &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "PUSH A stores accumulator without changing flags");
}

void test_pop_a() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x80; // MOV A,#$80
    ipl[2] = 0x2D; // PUSH A
    ipl[3] = 0xE8; ipl[4] = 0; // MOV A,#0
    ipl[5] = 0xAE; // POP A
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 4 &&
              cpu.step().cycles == 2 && cpu.step().cycles == 4 &&
              cpu.registers().a == 0x80 && cpu.registers().sp == 0 &&
              (cpu.registers().psw & 0x82U) == 0x02,
          "POP A restores accumulator and preserves flags");
}

void test_compare_a_absolute_indexed_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x30; // MOV X,#$30
    ipl[2] = 0xE8; ipl[3] = 0x80; // MOV A,#$80
    ipl[4] = 0x75; ipl[5] = 0xF0; ipl[6] = 0x01; // CMP A,$01f0+X
    bus.install_ipl(ipl);
    bus.spc_write(0x220, 0x81);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 2 &&
              cpu.step().cycles == 5 && cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0x83U) == 0x80,
          "CMP A,!abs+X crosses pages without changing A");
}

void test_compare_y_absolute() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 0x80; // MOV Y,#$80
    ipl[2] = 0x5E; ipl[3] = 0x20; ipl[4] = 0x02; // CMP Y,$0220
    bus.install_ipl(ipl);
    bus.spc_write(0x220, 0x81);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 4 &&
              cpu.registers().y == 0x80 &&
              (cpu.registers().psw & 0x83U) == 0x80,
          "CMP Y,!abs reads 16-bit address without changing Y");
}

void test_store_x_absolute() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x81; // MOV X,#$81
    ipl[2] = 0xC9; ipl[3] = 0x20; ipl[4] = 0x02; // MOV $0220,X
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 5 &&
              bus.spc_read(0x220) == 0x81 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV !abs,X stores X without changing flags");
}

void test_store_a_absolute_indexed_y() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 0x02; // MOV Y,#2
    ipl[2] = 0xE8; ipl[3] = 0x80; // MOV A,#$80
    ipl[4] = 0xD6; ipl[5] = 0xFE; ipl[6] = 0x1F; // MOV $1ffe+Y,A
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    const auto load_y = cpu.step();
    const auto load_a = cpu.step();
    check(load_y.supported && load_a.supported &&
              cpu.step().cycles == 6 && bus.spc_read(0x2000) == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV !abs+Y,A indexes the absolute address and preserves flags");
}

void test_lsr_absolute() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x4C; ipl[1] = 0x00; ipl[2] = 0x20;
    bus.install_ipl(ipl);
    bus.spc_write(0x2000, 1);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 5 && bus.spc_read(0x2000) == 0 &&
              (cpu.registers().psw & 0x83U) == 3,
          "LSR !abs moves bit 0 to carry and sets Z");
}

void test_dec_absolute() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8C; ipl[1] = 0x00; ipl[2] = 0x20;
    bus.install_ipl(ipl);
    bus.spc_write(0x2000, 0);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 5 && bus.spc_read(0x2000) == 0xFF &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "DEC !abs wraps and sets N");
}

void test_load_x_absolute() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE9; ipl[1] = 0x00; ipl[2] = 0x20;
    bus.install_ipl(ipl);
    bus.spc_write(0x2000, 0x80);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 4 && cpu.registers().x == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV X,!abs loads X and updates N/Z");
}

void test_adc_a_absolute() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x7F;
    ipl[2] = 0x80; // SETC
    ipl[3] = 0x85; ipl[4] = 0x00; ipl[5] = 0x20;
    bus.install_ipl(ipl);
    bus.spc_write(0x2000, 0);
    gameboy::SnesSpc700 cpu(bus);
    const auto a = cpu.step();
    const auto c = cpu.step();
    check(a.supported && c.supported && cpu.step().cycles == 4 &&
              cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0xCBU) == 0xC8,
          "ADC A,!abs reads operand and sets NZVHC");
}

void test_decrement_y() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 0x00; // MOV Y,#0
    ipl[2] = 0xDC; // DEC Y
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().cycles == 2 &&
              cpu.registers().y == 0xFF &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "DEC Y wraps and updates N/Z");
}

void test_adc_direct_immediate() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x42; // MOV A,#$42
    ipl[2] = 0x80; // SETC
    ipl[3] = 0x98; ipl[4] = 0x01; ipl[5] = 0x20; // ADC $20,#1
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0x7F);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    const auto set_c = cpu.step();
    check(load.supported && set_c.supported && cpu.step().cycles == 5 &&
              bus.spc_read(0x20) == 0x81 && cpu.registers().a == 0x42 &&
              (cpu.registers().psw & 0xCBU) == 0xC8,
          "ADC dp,#imm updates memory and NZVHC but preserves A");
}

void test_and_a_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0xF0; // MOV A,#$f0
    ipl[2] = 0x24; ipl[3] = 0x20; // AND A,$20
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0x0F);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    check(load.supported && cpu.step().cycles == 3 &&
              cpu.registers().a == 0 &&
              (cpu.registers().psw & 0x82U) == 0x02,
          "AND A,dp reads direct page and updates N/Z");
}

void test_push_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x81; // MOV X,#$81
    ipl[2] = 0x4D; // PUSH X
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    check(load.supported && cpu.step().cycles == 4 &&
              bus.spc_read(0x100) == 0x81 && cpu.registers().sp == 0xFF &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "PUSH X writes stack and preserves flags");
}

void test_mov_a_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x00; // MOV X,#0
    ipl[2] = 0xE8; ipl[3] = 0x80; // MOV A,#$80
    ipl[4] = 0x7D; // MOV A,X
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    const auto load_x = cpu.step();
    const auto load_a = cpu.step();
    check(load_x.supported && load_a.supported && cpu.step().cycles == 2 &&
              cpu.registers().a == 0 && (cpu.registers().psw & 0x82U) == 2,
          "MOV A,X copies X and updates N/Z");
}

void test_xcn_a() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x08; // MOV A,#8
    ipl[2] = 0x9F; // XCN A
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    check(load.supported && cpu.step().cycles == 5 &&
              cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "XCN A swaps nibbles and updates N/Z");
}

void test_tclr1_absolute() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x0F; // MOV A,#$0f
    ipl[2] = 0x4E; ipl[3] = 0x00; ipl[4] = 0x20; // TCLR1 $2000
    bus.install_ipl(ipl);
    bus.spc_write(0x2000, 0x8F);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    check(load.supported && cpu.step().cycles == 6 &&
              bus.spc_read(0x2000) == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "TCLR1 clears A bits and sets N/Z from A minus old memory");
}

void test_pop_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x42; // MOV X,#$42
    ipl[2] = 0x4D; // PUSH X
    ipl[3] = 0xCD; ipl[4] = 0; // MOV X,#0
    ipl[5] = 0xCE; // POP X
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    const auto push = cpu.step();
    const auto clear = cpu.step();
    check(load.supported && push.supported && clear.supported &&
              cpu.step().cycles == 4 && cpu.registers().x == 0x42 &&
              cpu.registers().sp == 0 &&
              (cpu.registers().psw & 0x82U) == 2,
          "POP X restores stack byte without changing flags");
}

void test_dec_direct_indexed_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x30; // MOV X,#$30
    ipl[2] = 0x9B; ipl[3] = 0xF0; // DEC $f0+X
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    check(load.supported && cpu.step().cycles == 5 &&
              bus.spc_read(0x20) == 0xFF &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "DEC dp+X wraps direct offset and updates N/Z");
}

void test_load_a_indirect_indexed_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x30; // MOV X,#$30
    ipl[2] = 0xE7; ipl[3] = 0xF0; // MOV A,[$f0+X]
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0x00);
    bus.spc_write(0x21, 0x20);
    bus.spc_write(0x2000, 0x80);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    check(load.supported && cpu.step().cycles == 6 &&
              cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV A,[dp+X] wraps pointer offset before indirect read");
}

void test_inc_direct_indexed_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x30; // MOV X,#$30
    ipl[2] = 0xBB; ipl[3] = 0xF0; // INC $f0+X
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0x7F);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    check(load.supported && cpu.step().cycles == 5 &&
              bus.spc_read(0x20) == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "INC dp+X wraps direct offset and updates N/Z");
}

void test_branch_if_negative() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x80; // MOV A,#$80
    ipl[2] = 0x30; ipl[3] = 0xFE; // BMI -2
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    check(load.supported && cpu.step().cycles == 4 &&
              cpu.registers().pc == 0xFFC2,
          "BMI branches on negative and charges taken-branch cycles");
}

void test_and_direct_immediate() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x38; ipl[1] = 0x0F; ipl[2] = 0x20;
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0xF0);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 5 && bus.spc_read(0x20) == 0 &&
              (cpu.registers().psw & 0x82U) == 2,
          "AND dp,#imm encodes immediate first and updates N/Z");
}

void test_or_a_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x01;
    ipl[2] = 0x04; ipl[3] = 0x20;
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0x80);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    check(load.supported && cpu.step().cycles == 3 &&
              cpu.registers().a == 0x81 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "OR A,dp reads direct page and updates N/Z");
}

void test_sbc_a_immediate() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0x80;
    ipl[2] = 0x80; // SETC
    ipl[3] = 0xA8; ipl[4] = 0x01; // SBC A,#1
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    const auto set_c = cpu.step();
    check(load.supported && set_c.supported && cpu.step().cycles == 2 &&
              cpu.registers().a == 0x7F &&
              (cpu.registers().psw & 0xCBU) == 0x41,
          "SBC A,#imm sets carry for no borrow and overflow on signed wrap");
}

void test_adc_a_absolute_indexed_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 2;
    ipl[2] = 0xE8; ipl[3] = 0x7F;
    ipl[4] = 0x80; // SETC
    ipl[5] = 0x95; ipl[6] = 0xFE; ipl[7] = 0x1F;
    bus.install_ipl(ipl);
    bus.spc_write(0x2000, 0);
    gameboy::SnesSpc700 cpu(bus);
    const auto load_x = cpu.step();
    const auto load_a = cpu.step();
    const auto set_c = cpu.step();
    check(load_x.supported && load_a.supported && set_c.supported &&
              cpu.step().cycles == 5 && cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0xCBU) == 0xC8,
          "ADC A,!abs+X indexes before read and updates NZVHC");
}

void test_ror_a() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 1;
    ipl[2] = 0x80; // SETC
    ipl[3] = 0x7C; // ROR A
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    const auto load = cpu.step();
    const auto set_c = cpu.step();
    check(load.supported && set_c.supported && cpu.step().cycles == 2 &&
              cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0x83U) == 0x81,
          "ROR A rotates carry into bit 7 and bit 0 into carry");
}

void test_or_direct_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x09; ipl[1] = 0x20; ipl[2] = 0x21;
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0x80);
    bus.spc_write(0x21, 1);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 6 && bus.spc_read(0x20) == 0x80 &&
              bus.spc_read(0x21) == 0x81 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "OR dp,dp encodes source first and writes destination");
}

void test_div_ya_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 3; // X = 3
    ipl[2] = 0x8D; ipl[3] = 0; // Y = 0
    ipl[4] = 0xE8; ipl[5] = 10; // A = 10
    ipl[6] = 0x9E; // DIV YA,X
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    const auto x = cpu.step();
    const auto y = cpu.step();
    const auto a = cpu.step();
    check(x.supported && y.supported && a.supported &&
              cpu.step().cycles == 12 &&
              cpu.registers().a == 3 && cpu.registers().y == 1 &&
              (cpu.registers().psw & 0xCAU) == 0,
          "DIV YA,X yields quotient and remainder with standard flags");
}

void test_subw_ya_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 0x10; // Y = $10
    ipl[2] = 0xE8; ipl[3] = 0; // A = 0
    ipl[4] = 0x9A; ipl[5] = 0x20; // SUBW YA,$20
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 1);
    bus.spc_write(0x21, 0);
    gameboy::SnesSpc700 cpu(bus);
    const auto y = cpu.step();
    const auto a = cpu.step();
    check(y.supported && a.supported && cpu.step().cycles == 5 &&
              cpu.registers().a == 0xFF && cpu.registers().y == 0x0F &&
              (cpu.registers().psw & 0xCBU) == 0x01,
          "SUBW YA,dp propagates low-byte borrow into high byte and H");
}

void test_addw_ya_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 0x0F; // Y = $0f
    ipl[2] = 0xE8; ipl[3] = 0xFF; // A = $ff
    ipl[4] = 0x7A; ipl[5] = 0x20; // ADDW YA,$20
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 1);
    bus.spc_write(0x21, 0);
    gameboy::SnesSpc700 cpu(bus);
    const auto y = cpu.step();
    const auto a = cpu.step();
    check(y.supported && a.supported && cpu.step().cycles == 5 &&
              cpu.registers().a == 0 && cpu.registers().y == 0x10 &&
              (cpu.registers().psw & 0xCBU) == 0x08,
          "ADDW YA,dp propagates low-byte carry into high-byte H");
}

void test_rol_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x80; // SETC
    ipl[1] = 0x2B; ipl[2] = 0x20; // ROL $20
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0x80);
    gameboy::SnesSpc700 cpu(bus);
    const auto set_c = cpu.step();
    check(set_c.supported && cpu.step().cycles == 4 &&
              bus.spc_read(0x20) == 1 &&
              (cpu.registers().psw & 0x83U) == 1,
          "ROL dp rotates carry into bit 0 and bit 7 into carry");
}

void test_sbc_a_absolute_indexed_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 2;
    ipl[2] = 0xE8; ipl[3] = 0x80;
    ipl[4] = 0x80; // SETC
    ipl[5] = 0xB5; ipl[6] = 0xFE; ipl[7] = 0x1F;
    bus.install_ipl(ipl);
    bus.spc_write(0x2000, 1);
    gameboy::SnesSpc700 cpu(bus);
    const auto x = cpu.step();
    const auto a = cpu.step();
    const auto c = cpu.step();
    check(x.supported && a.supported && c.supported &&
              cpu.step().cycles == 5 && cpu.registers().a == 0x7F &&
              (cpu.registers().psw & 0xCBU) == 0x41,
          "SBC A,!abs+X reads indexed address and sets NZVHC");
}

void test_load_y_direct_indexed_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x30;
    ipl[2] = 0xFB; ipl[3] = 0xF0;
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0x80);
    gameboy::SnesSpc700 cpu(bus);
    const auto x = cpu.step();
    check(x.supported && cpu.step().cycles == 4 &&
              cpu.registers().y == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV Y,dp+X wraps offset and updates N/Z");
}

void test_sbc_a_absolute_indexed_y() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 2;
    ipl[2] = 0xE8; ipl[3] = 0x80;
    ipl[4] = 0x80; // SETC
    ipl[5] = 0xB6; ipl[6] = 0xFE; ipl[7] = 0x1F;
    bus.install_ipl(ipl);
    bus.spc_write(0x2000, 1);
    gameboy::SnesSpc700 cpu(bus);
    const auto y = cpu.step();
    const auto a = cpu.step();
    const auto c = cpu.step();
    check(y.supported && a.supported && c.supported &&
              cpu.step().cycles == 5 && cpu.registers().a == 0x7F &&
              (cpu.registers().psw & 0xCBU) == 0x41,
          "SBC A,!abs+Y reads indexed address and sets NZVHC");
}

void test_adc_a_absolute_indexed_y() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 2;
    ipl[2] = 0xE8; ipl[3] = 0x7F;
    ipl[4] = 0x80; // SETC
    ipl[5] = 0x96; ipl[6] = 0xFE; ipl[7] = 0x1F;
    bus.install_ipl(ipl);
    bus.spc_write(0x2000, 0);
    gameboy::SnesSpc700 cpu(bus);
    const auto y = cpu.step();
    const auto a = cpu.step();
    const auto c = cpu.step();
    check(y.supported && a.supported && c.supported &&
              cpu.step().cycles == 5 && cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0xCBU) == 0xC8,
          "ADC A,!abs+Y reads indexed address and sets NZVHC");
}

void test_dec_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8B; ipl[1] = 0x20;
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 4 && bus.spc_read(0x20) == 0xFF &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "DEC dp wraps and updates N/Z");
}

void test_cbne_direct_indexed_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x30;
    ipl[2] = 0xE8; ipl[3] = 0x80;
    ipl[4] = 0xDE; ipl[5] = 0xF0; ipl[6] = 0xFE;
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 1);
    gameboy::SnesSpc700 cpu(bus);
    const auto x = cpu.step();
    const auto a = cpu.step();
    check(x.supported && a.supported && cpu.step().cycles == 8 &&
              cpu.registers().pc == 0xFFC5 &&
              (cpu.registers().psw & 0x83U) == 0x80,
          "CBNE dp+X branches on inequality without changing flags");
}

void test_store_a_direct_indexed_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x30; // MOV X,#$30
    ipl[2] = 0xE8; ipl[3] = 0x80; // MOV A,#$80
    ipl[4] = 0xD4; ipl[5] = 0xF0; // MOV $f0+X,A
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 2 &&
              cpu.step().cycles == 5 && bus.spc_read(0x20) == 0x80 &&
              cpu.registers().a == 0x80,
          "MOV dp+X,A wraps direct offset and stores A");
}

void test_store_x_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x81; // MOV X,#$81
    ipl[2] = 0xD8; ipl[3] = 0x30; // MOV $30,X
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 2 && cpu.step().cycles == 4 &&
              bus.spc_read(0x30) == 0x81 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV dp,X stores X without changing flags");
}

void test_load_a_indirect_x() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xCD; ipl[1] = 0x30; // MOV X,#$30
    ipl[2] = 0xE6;                 // MOV A,(X)
    bus.install_ipl(ipl);
    bus.spc_write(0x30, 0x80);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().cycles == 3 &&
              cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV A,(X) uses selected direct page and updates N/Z");
}

void test_compare_direct_to_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x69; ipl[1] = 0x20; ipl[2] = 0x21;
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 1);
    bus.spc_write(0x21, 2);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 6 &&
              (cpu.registers().psw & 0x83U) == 0x01 &&
              bus.spc_read(0x20) == 1 && bus.spc_read(0x21) == 2,
          "CMP dp,dp encodes source then destination and only changes flags");
}

void test_eor_a_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 0xFF; // MOV A,#$ff
    ipl[2] = 0x44; ipl[3] = 0x20; // EOR A,$20
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 0x7F);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().cycles == 3 &&
              cpu.registers().a == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "EOR A,dp XORs memory and updates N/Z");
}

void test_lsr_a() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xE8; ipl[1] = 1; // MOV A,#1
    ipl[2] = 0x5C;              // LSR A
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().cycles == 2 &&
              cpu.registers().a == 0 &&
              (cpu.registers().psw & 0x83U) == 0x03,
          "LSR A shifts low bit into carry and updates N/Z");
}

void test_ror_direct() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x80;              // SETC
    ipl[1] = 0x6B; ipl[2] = 0x20; // ROR $20
    bus.install_ipl(ipl);
    bus.spc_write(0x20, 1);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().cycles == 4 &&
              bus.spc_read(0x20) == 0x80 &&
              (cpu.registers().psw & 0x83U) == 0x81,
          "ROR dp rotates through carry and updates N/Z/C");
}

void test_load_y_absolute() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xEC; ipl[1] = 0x00; ipl[2] = 0x20; // MOV Y,$2000
    bus.install_ipl(ipl);
    bus.spc_write(0x2000, 0x80);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().cycles == 4 && cpu.registers().y == 0x80 &&
              (cpu.registers().psw & 0x82U) == 0x80,
          "MOV Y,absolute loads Y and updates N/Z");
}

void test_push_y() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0x8D; ipl[1] = 0x42; // MOV Y,#$42
    ipl[2] = 0x6D;                // PUSH Y
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    check(cpu.step().supported && cpu.step().cycles == 4 &&
              bus.spc_read(0x100) == 0x42 && cpu.registers().sp == 0xFF,
          "PUSH Y writes stack byte and wraps stack pointer");
}

} // namespace

int main() {
    test_resumable_bus();
    test_write_cycle_observation();
    test_cycle_bus_timers();
    test_adc_a_absolute();
    test_load_x_absolute();
    test_dec_absolute();
    test_lsr_absolute();
    test_cbne_direct_indexed_x();
    test_dec_direct();
    test_adc_a_absolute_indexed_y();
    test_sbc_a_absolute_indexed_y();
    test_load_y_direct_indexed_x();
    test_sbc_a_absolute_indexed_x();
    test_rol_direct();
    test_addw_ya_direct();
    test_subw_ya_direct();
    test_div_ya_x();
    test_or_direct_direct();
    test_ror_a();
    test_adc_a_absolute_indexed_x();
    test_sbc_a_immediate();
    test_or_a_direct();
    test_and_direct_immediate();
    test_branch_if_negative();
    test_inc_direct_indexed_x();
    test_load_a_indirect_indexed_x();
    test_dec_direct_indexed_x();
    test_pop_x();
    test_tclr1_absolute();
    test_xcn_a();
    test_mov_a_x();
    test_push_x();
    test_and_a_direct();
    test_adc_direct_immediate();
    test_decrement_y();
    test_store_a_absolute_indexed_y();
    test_synthetic_upload();
    test_upload_addressing_and_trap();
    test_boot_arithmetic_and_branch_flags();
    test_word_transfer();
    test_direct_page_selection();
    test_indexed_post_increment_store();
    test_compare_x_immediate();
    test_increment_x_wrap();
    test_increment_a_wrap();
    test_absolute_indexed_store();
    test_indirect_indexed_store();
    test_call_return_stack();
    test_load_y_immediate();
    test_store_y_absolute();
    test_store_a_absolute();
    test_load_a_absolute();
    test_load_a_absolute_y();
    test_compare_a_direct();
    test_equal_branch();
    test_and_a_immediate();
    test_eor_a_immediate();
    test_direct_bit_branches();
    test_decrement_y_branch();
    test_or_a_immediate();
    test_asl_a();
    test_set_carry();
    test_adc_a_immediate();
    test_absolute_jump();
    test_set_direct_bit();
    test_multiply_ya();
    test_load_a_absolute_x();
    test_move_a_to_y();
    test_load_x_direct();
    test_compare_y_immediate();
    test_compare_a_immediate();
    test_branch_carry_set();
    test_branch_carry_clear();
    test_pop_y();
    test_load_a_direct_indexed_x();
    test_shift_direct_left();
    test_test_and_set_absolute();
    test_or_direct_immediate();
    test_decrement_direct_and_branch();
    test_shift_direct_right();
    test_load_a_indirect_direct_y();
    test_increment_word_direct();
    test_push_a();
    test_pop_a();
    test_compare_a_absolute_indexed_x();
    test_compare_y_absolute();
    test_store_x_absolute();
    test_store_a_direct_indexed_x();
    test_store_x_direct();
    test_load_a_indirect_x();
    test_compare_direct_to_direct();
    test_eor_a_direct();
    test_lsr_a();
    test_ror_direct();
    test_load_y_absolute();
    test_push_y();
    return failures == 0 ? 0 : 1;
}
