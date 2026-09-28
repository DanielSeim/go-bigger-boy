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
