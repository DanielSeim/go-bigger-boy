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

} // namespace

int main() {
    test_synthetic_upload();
    test_upload_addressing_and_trap();
    test_boot_arithmetic_and_branch_flags();
    test_word_transfer();
    return failures == 0 ? 0 : 1;
}
