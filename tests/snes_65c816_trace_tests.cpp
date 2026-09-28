#include "snes_65c816_trace_cpu.hpp"
#include "gameboy/snes_spc700.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures{};

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

gameboy::SgbProgramRom program(const std::vector<std::uint8_t>& code) {
    std::vector<std::uint8_t> bytes(0x40000);
    bytes[0x7FD5] = 0x20;
    bytes[0x7FFC] = 0x04;
    bytes[0x7FFD] = 0x81;
    for (std::size_t i = 0; i < code.size(); ++i) bytes[0x0104 + i] = code[i];
    return gameboy::SgbProgramRom(std::move(bytes));
}

void test_native_width_and_apu_mapping() {
    const auto rom = program({
        0x78,             // SEI
        0x18, 0xFB,       // CLC; XCE: enter native mode
        0xC2, 0x10,       // REP #$10: 16-bit X/Y
        0xA2, 0x34, 0x12, // LDX #$1234
        0x9A,             // TXS
        0xE2, 0x10,       // SEP #$10: 8-bit X/Y
        0xA9, 0x5A,       // LDA #$5A
        0x8D, 0x40, 0x21, // STA $2140
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 9; ++i) {
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "native startup instructions are supported");
    }
    check(!cpu.registers().e && cpu.registers().s == 0x1234 &&
              cpu.registers().x == 0x34,
          "native stack survives index-width narrowing");
    check(cpu.apu_write_count() == 1 &&
              cpu.apu_write(0).step == 9 &&
              cpu.apu_write(0).port == 0 &&
              cpu.apu_write(0).value == 0x5A &&
              apu.spc_read(0xF4) == 0x5A,
          "SNES CPU write reaches the independent APU input port");
}

void test_fail_closed() {
    auto rom = program({0xAD, 0x3E, 0x21}); // LDA $213E: unmodeled sprite status
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    const auto read = cpu.step();
    check(read.error == sgb_test::Snes65c816TraceCpu::Error::unsupported_read &&
              read.address == 0x00213E && cpu.apu_write_count() == 0,
          "unknown control-flow-relevant I/O reads trap");

    rom = program({0x02}); // COP is not silently treated as NOP
    sgb_test::Snes65c816TraceCpu unknown(rom, apu);
    const auto opcode = unknown.step();
    check(opcode.error == sgb_test::Snes65c816TraceCpu::Error::unsupported_opcode &&
              opcode.opcode == 0x02 && opcode.pc == 0x8104,
          "unsupported opcode reports its original PC");

    rom = program({0x80, 0xFE}); // BRA self while SPC fetches unsupported STOP
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xFF;
    apu.install_ipl(ipl);
    gameboy::SnesSpc700 spc(apu);
    sgb_test::Snes65c816TraceCpu coupled(rom, apu, &spc);
    sgb_test::Snes65c816TraceCpu::StepResult synchronized{};
    for (unsigned i = 0; i < 16; ++i) {
        synchronized = coupled.step();
        if (synchronized.error != sgb_test::Snes65c816TraceCpu::Error::none)
            break;
    }
    check(synchronized.error ==
              sgb_test::Snes65c816TraceCpu::Error::unsupported_spc_opcode &&
              synchronized.address == 0xFFC0,
          "SPC error during 65C816 opcode fetch is not mislabeled as a host opcode");
}

void test_icd_wram_port_dma() {
    struct Stream final : sgb_test::SnesIcdTraceSource {
        unsigned reads{};
        bool read(std::uint16_t address, std::uint64_t,
                  std::uint8_t& value) noexcept override {
            if (address != 0x7800) return false;
            value = static_cast<std::uint8_t>(0xA0U + reads++);
            return true;
        }
        bool write(std::uint16_t, std::uint64_t,
                   std::uint8_t) noexcept override { return false; }
    } source;
    std::vector<std::uint8_t> code;
    const auto store = [&code](const std::uint16_t address,
                               const std::uint8_t value) {
        code.insert(code.end(), {0xA9, value, 0x8D,
            static_cast<std::uint8_t>(address),
            static_cast<std::uint8_t>(address >> 8)});
    };
    store(0x2181, 0xFE);
    store(0x2182, 0xFF);
    store(0x2183, 0x01);
    store(0x4300, 0x08); // A-bus to B-bus, fixed source, mode 0.
    store(0x4301, 0x80); // WRAM data port.
    store(0x4302, 0x00);
    store(0x4303, 0x78);
    store(0x4304, 0x00);
    store(0x4305, 0x03);
    store(0x4306, 0x00);
    store(0x420B, 0x01);
    store(0x2180, 0xA3); // Direct port write after DMA wraps WMADD.
    gameboy::SnesApuBus apu;
    const auto rom = program(code);
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    cpu.set_icd_source(&source);
    for (unsigned i = 0; i < 24; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "ICD WRAM DMA setup and transfer execute");
    check(source.reads == 3 && cpu.debug_wram_byte(0x1FFFE) == 0xA0 &&
              cpu.debug_wram_byte(0x1FFFF) == 0xA1 &&
              cpu.debug_wram_byte(0) == 0xA2 &&
              cpu.debug_wram_byte(1) == 0xA3 &&
              cpu.wram_port_address() == 2,
          "fixed ICD stream DMA and direct WMDATA wrap at 128 KiB");
    check(cpu.dma_register(0, 5) == 0 && cpu.dma_register(0, 6) == 0,
          "completed DMA clears transfer count");

    code.clear();
    store(0x4300, 0x00); // Incrementing source is not modeled here.
    store(0x4301, 0x80);
    store(0x4302, 0x00);
    store(0x4303, 0x78);
    store(0x4305, 0x01);
    store(0x420B, 0x01);
    const auto unsupported_rom = program(code);
    sgb_test::Snes65c816TraceCpu unsupported(unsupported_rom, apu);
    unsupported.set_icd_source(&source);
    for (unsigned i = 0; i < 11; ++i)
        check(unsupported.step().error ==
                  sgb_test::Snes65c816TraceCpu::Error::none,
              "unsupported DMA setup executes");
    const auto result = unsupported.step();
    check(result.error ==
              sgb_test::Snes65c816TraceCpu::Error::unsupported_write &&
              result.address == 0x420B && source.reads == 3,
          "unsupported WRAM DMA fails before consuming ICD bytes");
}

void test_host_math_results() {
    const auto rom = program({
        0xA9, 0x12, 0x8D, 0x02, 0x42, // Multiplicand 18.
        0xA9, 0x34, 0x8D, 0x03, 0x42, // Multiplier 52.
        0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
        0xAD, 0x16, 0x42,             // Product low = $A8.
        0xAD, 0x17, 0x42,             // Product high = $03.
        0xA9, 0x34, 0x8D, 0x04, 0x42,
        0xA9, 0x12, 0x8D, 0x05, 0x42,
        0xA9, 0x10, 0x8D, 0x06, 0x42,
        0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
        0xAD, 0x14, 0x42,             // Quotient low = $23.
        0xAD, 0x15, 0x42,             // Quotient high = $01.
        0xAD, 0x16, 0x42,             // Remainder = $04.
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 12; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "host multiplication setup and delay execute");
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0xA8,
          "host multiplication low byte is visible");
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0x03,
          "host multiplication high byte is visible");
    for (unsigned i = 0; i < 14; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "host division setup and delay execute");
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0x23,
          "host division quotient low byte is visible");
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0x01,
          "host division quotient high byte is visible");
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0x04,
          "host division remainder is visible");

    const auto early_rom = program({
        0xA9, 0x03, 0x8D, 0x02, 0x42,
        0xA9, 0x07, 0x8D, 0x03, 0x42,
        0xAD, 0x16, 0x42,
    });
    sgb_test::Snes65c816TraceCpu early(early_rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(early.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "early host math setup executes");
    check(early.step().error ==
              sgb_test::Snes65c816TraceCpu::Error::unsupported_read,
          "intermediate hardware math result fails closed");

    const auto unknown_quotient_rom = program({
        0xA9, 0x03, 0x8D, 0x02, 0x42,
        0xA9, 0x07, 0x8D, 0x03, 0x42,
        0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
        0xAD, 0x14, 0x42,
    });
    sgb_test::Snes65c816TraceCpu unknown_quotient(unknown_quotient_rom, apu);
    for (unsigned i = 0; i < 12; ++i)
        check(unknown_quotient.step().error ==
                  sgb_test::Snes65c816TraceCpu::Error::none,
              "unknown quotient setup executes");
    check(unknown_quotient.step().error ==
              sgb_test::Snes65c816TraceCpu::Error::unsupported_read,
          "multiplication does not fabricate an uninitialized quotient");
}

void test_lsr_direct_page() {
    const auto rom = program({
        0xA9, 0x03, 0x85, 0x20, // LDA #3; STA $20
        0x46, 0x20,             // LSR $20 -> 1, carry set
        0xA5, 0x20,             // LDA $20
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "LSR direct-page fixture executes");
    check(cpu.debug_wram_byte(0x20) == 1 &&
              (cpu.registers().a & 0xFFU) == 1 &&
              (cpu.registers().p & 1U) != 0,
          "LSR direct-page writes shifted value and old bit zero to carry");
}

void test_asl_direct_page() {
    const auto rom = program({
        0xA9, 0x81, 0x85, 0x20, // LDA #$81; STA $20
        0x06, 0x20,             // ASL $20 -> 2, carry set
        0xA5, 0x20,
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "ASL direct-page fixture executes");
    check(cpu.debug_wram_byte(0x20) == 2 &&
              (cpu.registers().a & 0xFFU) == 2 &&
              (cpu.registers().p & 1U) != 0,
          "ASL direct-page writes shifted value and old top bit to carry");
}

void test_icd_source_contract() {
    struct Source final : sgb_test::SnesIcdTraceSource {
        std::uint16_t read_address{};
        std::uint16_t write_address{};
        std::uint64_t read_clock{};
        std::uint64_t write_clock{};
        std::uint8_t written{};
        bool permit_read{true};
        bool permit_write{true};

        bool read(const std::uint16_t address, const std::uint64_t clock,
                  std::uint8_t& value) noexcept override {
            read_address = address;
            read_clock = clock;
            value = 0x5A;
            return permit_read;
        }
        bool write(const std::uint16_t address, const std::uint64_t clock,
                   const std::uint8_t value) noexcept override {
            write_address = address;
            write_clock = clock;
            written = value;
            return permit_write;
        }
    } source;
    const auto rom = program({0xAD, 0x00, 0x60, 0x8D, 0x03, 0x60});
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    cpu.set_icd_source(&source);
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0x5A &&
              source.read_address == 0x6000 && source.read_clock != 0,
          "ICD status read uses only the attached source and SNES clock");
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              source.write_address == 0x6003 && source.written == 0x5A &&
              source.write_clock >= source.read_clock,
          "ICD control write reaches the attached source");

    source.permit_read = false;
    sgb_test::Snes65c816TraceCpu missing(rom, apu);
    missing.set_icd_source(&source);
    const auto read = missing.step();
    check(read.error == sgb_test::Snes65c816TraceCpu::Error::unsupported_read &&
              read.address == 0x6000,
          "attached ICD source cannot invent missing status data");

    source.permit_write = false;
    const auto write_rom = program({0xA9, 0x80, 0x8D, 0x03, 0x60});
    sgb_test::Snes65c816TraceCpu rejected(write_rom, apu);
    rejected.set_icd_source(&source);
    check(rejected.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
          "write-rejection setup executes");
    const auto write = rejected.step();
    check(write.error == sgb_test::Snes65c816TraceCpu::Error::unsupported_write &&
              write.address == 0x6003,
          "attached ICD source can reject unsupported controls");
}

void test_branch_carry_set() {
    const auto rom = program({
        0xA9, 0x00,       // LDA #0
        0x38,             // SEC
        0xB0, 0x02,       // BCS +2
        0xA9, 0xFF,       // skipped LDA
        0xA9, 0x42,       // LDA #$42
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.registers().pc == 0x810B &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0x42,
          "BCS takes the positive branch when carry is set");
}

void test_compare_y_immediate() {
    const auto rom = program({0xA0, 0x10, 0xC0, 0x10});
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.registers().y == 0x10 &&
              (cpu.registers().p & 0x83U) == 0x03,
          "CPY #imm compares Y without modifying it");
}

void test_compare_absolute_indexed_x() {
    const auto rom = program({
        0xA2, 0x01,       // LDX #1
        0xA9, 0x42,       // LDA #$42
        0x8D, 0x10, 0x00, // STA $0010
        0xA9, 0x40,       // LDA #$40
        0xDD, 0x0F, 0x00, // CMP $000F,X
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "CMP abs,X setup executes");
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0x40 &&
              (cpu.registers().p & 0x83U) == 0x80,
          "CMP abs,X compares indexed data and preserves A");
}

void test_compare_absolute_indexed_y() {
    const auto rom = program({
        0xA0, 0x01,       // LDY #1
        0xA9, 0x42,       // LDA #$42
        0x8D, 0x10, 0x00, // STA $0010
        0xA9, 0x40,       // LDA #$40
        0xD9, 0x0F, 0x00, // CMP $000F,Y
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 5; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "CMP abs,Y program executes");
    check((cpu.registers().a & 0xFFU) == 0x40 &&
              (cpu.registers().p & 0x83U) == 0x80,
          "CMP abs,Y compares indexed data and preserves A");
}

void test_test_and_set_direct() {
    const auto rom = program({
        0xA9, 0x80, // LDA #$80
        0x04, 0x10, // TSB $10, Z=1
        0x04, 0x10, // TSB $10, Z=0
        0xA5, 0x10, // LDA $10
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().p & 0x02U) != 0 &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().p & 0x02U) == 0 &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0x80,
          "TSB dp tests old bits then sets masked bits");
}

void test_indexed_indirect_jump() {
    const auto rom = program({
        0xA2, 0x01,       // LDX #1
        0x7C, 0x0F, 0x81, // JMP ($810F,X), pointer at $8110
        0, 0, 0, 0, 0, 0, 0, // pad to $8110
        0x20, 0x81,       // target $8120
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.registers().pc == 0x8120,
          "JMP (abs,X) reads pointer in current program bank");
}

void test_ppu_counter_latch() {
    const auto rom = program({
        0xAD, 0x37, 0x21, // LDA $2137, open bus = last operand $21
        0xAD, 0x3C, 0x21, // LDA $213C, H low
        0xAD, 0x3C, 0x21, // LDA $213C, H high
        0xAD, 0x3F, 0x21, // LDA $213F, reset H/V read toggles
        0xAD, 0x3C, 0x21, // LDA $213C, H low again
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0x21,
          "$2137 latches counters and reads undriven CPU data bus");
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "latched H counter and status reads are supported");
}

void test_subtract_absolute() {
    const auto rom = program({
        0xA9, 0x01,       // LDA #1
        0x8D, 0x10, 0x00, // STA $10
        0xA9, 0x80,       // LDA #$80
        0x38,             // SEC
        0xED, 0x10, 0x00, // SBC $10 -> $7f
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 5; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "SBC abs program executes");
    check((cpu.registers().a & 0xFFU) == 0x7F &&
              (cpu.registers().p & 0xC1U) == 0x41,
          "SBC abs reads operand and sets V/C without borrow");
}

void test_indexed_indirect_subroutine() {
    const auto rom = program({
        0xA2, 0x01,
        0xFC, 0x0F, 0x81, // JSR ($810F,X)
        0, 0, 0, 0, 0, 0, 0,
        0x20, 0x81,
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.registers().pc == 0x8120 && cpu.registers().s == 0x01FD,
          "JSR (abs,X) reads program-bank pointer and pushes return address");
}

void test_bit_immediate() {
    const auto rom = program({
        0xA9, 0x80,
        0x89, 0x01, // BIT #1 -> Z
        0x89, 0x80, // BIT #$80 -> !Z
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().p & 0x82U) == 0x82 &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().p & 0x82U) == 0x80,
          "BIT #imm changes Z but preserves N");
}

void test_add_absolute() {
    const auto rom = program({
        0xA9, 0x01,       // LDA #1
        0x8D, 0x10, 0x00, // STA $10
        0xA9, 0x7F,       // LDA #$7f
        0x18,             // CLC
        0x6D, 0x10, 0x00, // ADC $10 -> $80
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 5; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "ADC abs program executes");
    check((cpu.registers().a & 0xFFU) == 0x80 &&
              (cpu.registers().p & 0xC1U) == 0xC0,
          "ADC abs reads operand and sets N/V");
}

void test_decrement_absolute() {
    const auto rom = program({
        0xA9, 0x00,
        0x8D, 0x10, 0x00,
        0xCE, 0x10, 0x00,
        0xAD, 0x10, 0x00,
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "DEC abs program executes");
    check((cpu.registers().a & 0xFFU) == 0xFF &&
              (cpu.registers().p & 0x80U) != 0,
          "DEC abs wraps memory and updates N");
}

void test_load_x_direct() {
    const auto rom = program({
        0xA9, 0x80,
        0x85, 0x10,
        0xA6, 0x10,
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 3; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "LDX dp program executes");
    check(cpu.registers().x == 0x80 &&
              (cpu.registers().p & 0x82U) == 0x80,
          "LDX dp loads X and updates N/Z");
}

void test_load_y_absolute_indexed_x() {
    const auto rom = program({
        0xA2, 0x01,
        0xA9, 0x80,
        0x8D, 0x10, 0x00,
        0xBC, 0x0F, 0x00,
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "LDY abs,X program executes");
    check(cpu.registers().y == 0x80 &&
              (cpu.registers().p & 0x82U) == 0x80,
          "LDY abs,X reads indexed address and updates N/Z");
}

void test_or_indirect_indexed_y() {
    const auto rom = program({
        0xA0, 0x01,       // Y=1
        0xA9, 0x10,
        0x85, 0x20,       // pointer low=$10
        0xA9, 0x00,
        0x85, 0x21,       // pointer high=0
        0xA9, 0x80,
        0x85, 0x11,       // data at pointer+Y
        0xA9, 0x01,
        0x11, 0x20,       // ORA ($20),Y
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 9; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "ORA (dp),Y program executes");
    check((cpu.registers().a & 0xFFU) == 0x81 &&
              (cpu.registers().p & 0x82U) == 0x80,
          "ORA (dp),Y reads indirect indexed operand and updates N/Z");
}

void test_subtract_direct() {
    const auto rom = program({
        0xA9, 0x01,
        0x85, 0x10,
        0xA9, 0x80,
        0x38,
        0xE5, 0x10,
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 5; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "SBC dp program executes");
    check((cpu.registers().a & 0xFFU) == 0x7F &&
              (cpu.registers().p & 0xC1U) == 0x41,
          "SBC dp reads direct operand and sets V/C");
}

void test_and_direct() {
    const auto rom = program({
        0xA9, 0x0F,
        0x85, 0x10,
        0xA9, 0xF0,
        0x25, 0x10,
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "AND dp program executes");
    check((cpu.registers().a & 0xFFU) == 0 &&
              (cpu.registers().p & 0x02U) != 0,
          "AND dp reads direct operand and sets Z");
}

void test_xor_direct() {
    const auto rom = program({
        0xA9, 0xF0,
        0x85, 0x10,
        0xA9, 0xF0,
        0x45, 0x10,
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "EOR dp program executes");
    check((cpu.registers().a & 0xFFU) == 0 &&
              (cpu.registers().p & 0x02U) != 0,
          "EOR dp reads direct operand and sets Z");
}

void test_rotate_right_accumulator() {
    const auto rom = program({
        0xA9, 0x01,
        0x38, // SEC
        0x6A, // ROR A -> $80, C=1
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 3; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "ROR A program executes");
    check((cpu.registers().a & 0xFFU) == 0x80 &&
              (cpu.registers().p & 0x83U) == 0x81,
          "ROR A rotates carry through accumulator and updates N/Z/C");
}

void test_increment_absolute_indexed_x() {
    const auto rom = program({
        0xA2, 0x01,
        0xA9, 0x7F,
        0x8D, 0x10, 0x00,
        0xFE, 0x0F, 0x00,
        0xAD, 0x10, 0x00,
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 5; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "INC abs,X program executes");
    check((cpu.registers().a & 0xFFU) == 0x80 &&
              (cpu.registers().p & 0x82U) == 0x80,
          "INC abs,X modifies indexed memory and updates N/Z");
}

void test_or_long_indirect_indexed_y() {
    const auto rom = program({
        0xA0, 0x01,
        0xA9, 0x10, 0x85, 0x20,
        0xA9, 0x00, 0x85, 0x21, 0x85, 0x22,
        0xA9, 0x80, 0x85, 0x11,
        0xA9, 0x01,
        0x17, 0x20,
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 10; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "ORA [dp],Y program executes");
    check((cpu.registers().a & 0xFFU) == 0x81 &&
              (cpu.registers().p & 0x82U) == 0x80,
          "ORA [dp],Y reads long indirect operand and updates N/Z");
}

void test_transfer_y_to_x() {
    const auto rom = program({
        0xA0, 0x80,
        0xBB,
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.registers().x == 0x80 &&
              (cpu.registers().p & 0x82U) == 0x80,
          "TYX copies Y to X and updates N/Z");
}

void test_transfer_x_to_y() {
    const auto rom = program({
        0xA2, 0x80,
        0x9B,
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.registers().y == 0x80 &&
              (cpu.registers().p & 0x82U) == 0x80,
          "TXY copies X to Y and updates N/Z");
}

void test_store_absolute_indexed_y() {
    const auto rom = program({
        0xA0, 0x01,       // LDY #1
        0xA9, 0x42,       // LDA #$42
        0x99, 0x0F, 0x00, // STA $000F,Y
        0xAD, 0x10, 0x00, // LDA $0010
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "STA abs,Y test executes");
    check((cpu.registers().a & 0xFFU) == 0x42,
          "STA abs,Y writes the indexed address");
}

void test_or_direct() {
    const auto rom = program({
        0xA9, 0x80,       // LDA #$80
        0x8D, 0x10, 0x00, // STA $0010
        0xA9, 0x01,       // LDA #1
        0x05, 0x10,       // ORA $10
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "ORA dp test executes");
    check((cpu.registers().a & 0xFFU) == 0x81 &&
              (cpu.registers().p & 0x82U) == 0x80,
          "ORA dp reads direct page and sets N/Z");
}

void test_hv_irq_latch() {
    std::vector<std::uint8_t> code{
        0xA9, 0x3C, 0x8D, 0x07, 0x42, // HTIME=$003c
        0xA9, 0x00, 0x8D, 0x08, 0x42,
        0x8D, 0x09, 0x42, 0x8D, 0x0A, 0x42, // VTIME=0
        0xA9, 0x30, 0x8D, 0x00, 0x42, // HV IRQ enabled
    };
    for (unsigned i = 0; i < 48; ++i) code.push_back(0x18); // CLC
    code.insert(code.end(), {0xAD, 0x11, 0x42, 0xAD, 0x11, 0x42});
    const auto rom = program(code);
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 8 + 48; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "HV timer setup executes");
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0x80 &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0,
          "$4211 latches the V/H timer event and clears on read");
}

void test_cli_clears_interrupt_disable() {
    const auto rom = program({0x78, 0x58}); // SEI; CLI
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().p & 4U) != 0 &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().p & 4U) == 0,
          "CLI clears the interrupt-disable flag");
}

void test_irq_entry_and_rti() {
    std::vector<std::uint8_t> image(0x40000);
    image[0x7FD5] = 0x20;
    image[0x7FFC] = 0x04; image[0x7FFD] = 0x81;
    image[0x7FFE] = 0x20; image[0x7FFF] = 0x81; // emulation IRQ vector
    const std::vector<std::uint8_t> main{
        0xA9, 0x50, 0x8D, 0x07, 0x42, // HTIME=80
        0xA9, 0x00, 0x8D, 0x08, 0x42,
        0x8D, 0x09, 0x42, 0x8D, 0x0A, 0x42, // VTIME=0
        0xA9, 0x30, 0x8D, 0x00, 0x42, // enable HV IRQ
        0x58, 0x80, 0xFE,             // CLI; BRA self
    };
    for (std::size_t i = 0; i < main.size(); ++i) image[0x104 + i] = main[i];
    const std::vector<std::uint8_t> handler{
        0xAD, 0x11, 0x42, // LDA $4211 clears timer IRQ
        0xA9, 0x42,       // LDA #$42
        0x40,             // RTI
    };
    for (std::size_t i = 0; i < handler.size(); ++i) image[0x120 + i] = handler[i];
    const gameboy::SgbProgramRom rom(std::move(image));
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    bool returned = false;
    for (unsigned i = 0; i < 200; ++i) {
        if (cpu.step().error != sgb_test::Snes65c816TraceCpu::Error::none) break;
        if (cpu.irq_entries() == 1 && cpu.registers().pc == 0x811A &&
            (cpu.registers().a & 0xFFU) == 0x42) {
            returned = true;
            break;
        }
    }
    check(returned && cpu.irq_entries() == 1,
          "HV IRQ vectors to handler and RTI restores the interrupted PC");
}

void test_push_x() {
    const auto rom = program({
        0xA2, 0x42, // LDX #$42
        0xDA,       // PHX
        0xA9, 0x00, // LDA #0
        0x68,       // PLA
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "PHX test executes");
    check((cpu.registers().a & 0xFFU) == 0x42 &&
              cpu.registers().s == 0x01FF,
          "PHX pushes X in index width and restores stack with PLA");
}

void test_push_y() {
    const auto rom = program({
        0xA0, 0x43, // LDY #$43
        0x5A,       // PHY
        0xA9, 0x00, // LDA #0
        0x68,       // PLA
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "PHY test executes");
    check((cpu.registers().a & 0xFFU) == 0x43 &&
              cpu.registers().s == 0x01FF,
          "PHY pushes Y in index width and restores stack with PLA");
}

void test_pull_y() {
    const auto rom = program({
        0xA0, 0x80, // LDY #$80
        0x5A,       // PHY
        0xA0, 0x00, // LDY #0
        0x7A,       // PLY
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "PLY test executes");
    check(cpu.registers().y == 0x80 && cpu.registers().s == 0x01FF &&
              (cpu.registers().p & 0x82U) == 0x80,
          "PLY restores Y and updates N/Z");
}

void test_pull_x() {
    const auto rom = program({
        0xA2, 0x80, // LDX #$80
        0xDA,       // PHX
        0xA2, 0x00, // LDX #0
        0xFA,       // PLX
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 4; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "PLX test executes");
    check(cpu.registers().x == 0x80 && cpu.registers().s == 0x01FF &&
              (cpu.registers().p & 0x82U) == 0x80,
          "PLX restores X and updates N/Z");
}

void test_subtract_immediate() {
    const auto rom = program({
        0xA9, 0x10, // LDA #$10
        0x38,       // SEC
        0xE9, 0x01, // SBC #1 -> $0f, carry
        0xE9, 0x10, // SBC #$10 -> $ff, no carry
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0x0F &&
              (cpu.registers().p & 1U) != 0 &&
              cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none &&
              (cpu.registers().a & 0xFFU) == 0xFF &&
              (cpu.registers().p & 0x81U) == 0x80,
          "SBC immediate handles carry/no-borrow in 8-bit binary mode");
}

void test_ntsc_status_boundaries() {
    sgb_test::SnesTraceTiming timing;
    check(timing.hvbjoy() == 0x40 && timing.stat78() == 0x02,
          "reset starts in H-blank, field zero, NTSC PPU2 version 2");
    timing.advance(4);
    check(timing.hvbjoy() == 0, "H-blank clears at H=1 dot");
    timing.advance(1092);
    check(timing.hvbjoy() == 0x40, "H-blank starts at H=274 dots");
    timing.advance(225ULL * 1364 - timing.clocks());
    check(timing.line() == 225 && timing.horizontal_clock() == 0 &&
              (timing.hvbjoy() & 0x80U) != 0 && timing.rdnmi() == 0x82 &&
              timing.rdnmi() == 0x02,
          "V-blank and read-to-clear NMI status begin on line 225");
    timing.write_autojoy(1);
    timing.advance(297);
    check((timing.hvbjoy() & 1U) == 0, "first auto-read has not started at H=297");
    timing.advance(1);
    check((timing.hvbjoy() & 1U) != 0, "first auto-read starts at H=298");
    timing.advance(4224);
    check((timing.hvbjoy() & 1U) == 0, "auto-read busy clears after 4224 clocks");
    timing.advance(262ULL * 1364 - timing.clocks());
    check(timing.line() == 0 && timing.field() && timing.stat78() == 0x82 &&
              (timing.hvbjoy() & 0x80U) == 0,
          "field toggles and V-blank clears on frame wrap");
    const auto second_vblank = timing.clocks() + 225ULL * 1364;
    const auto earliest = second_vblank + 130;
    const auto first_phase = (225ULL * 1364 + 298) % 256;
    const auto next_start = earliest + (first_phase + 256 - earliest % 256) % 256;
    timing.advance(next_start - timing.clocks() - 1);
    check((timing.hvbjoy() & 1U) == 0, "second auto-read has not started early");
    timing.advance(1);
    check((timing.hvbjoy() & 1U) != 0, "second auto-read keeps the 256-clock phase");
    timing.advance(4224);
    check((timing.hvbjoy() & 1U) == 0, "second auto-read ends after 4224 clocks");
    timing.advance(240ULL * 1364 + 262ULL * 1364 - timing.clocks());
    check(timing.line() == 240 && timing.field(), "odd field reaches line 240");
    timing.advance(1360);
    check(timing.line() == 241 && timing.horizontal_clock() == 0,
          "odd field line 240 has the short 1360-clock length");
}

void test_status_latch_overscan_and_refresh() {
    sgb_test::SnesTraceTiming timing;
    timing.write_overscan(4);
    timing.advance(225ULL * 1364);
    check((timing.hvbjoy() & 0x80U) == 0, "overscan defers V-blank past line 225");
    timing.advance(15ULL * 1364);
    check((timing.hvbjoy() & 0x80U) != 0, "overscan V-blank begins at line 240");
    timing.write_latch(0);
    check((timing.stat78() & 0x40U) != 0, "counter latch sets STAT78 latch flag");
    timing.write_latch(0x80);
    check((timing.stat78() & 0x40U) != 0 &&
              (timing.stat78() & 0x40U) == 0,
          "STAT78 read clears the latch flag when latch is enabled");

    sgb_test::SnesTraceTiming refresh;
    refresh.advance(532);
    refresh.cpu_cycle(12);
    check(refresh.clocks() == 584 && refresh.horizontal_clock() == 584,
          "CPU access crossing H=538 incurs a 40-clock WRAM refresh pause");
}

void test_16_bit_direct_page_store() {
    const auto rom = program({
        0x18, 0xFB,       // CLC; XCE: enter native mode
        0xC2, 0x20,       // REP #$20: 16-bit accumulator
        0xA9, 0x34, 0x12, // LDA #$1234
        0x85, 0x10,       // STA $10, both bytes
        0xA9, 0x00, 0x00, // LDA #$0000
        0xAD, 0x10, 0x00, // LDA $0010
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 7; ++i) {
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "16-bit direct-page program executes");
    }
    check(cpu.registers().a == 0x1234,
          "16-bit direct-page store preserves both accumulator bytes");
}

void test_post_upload_arithmetic_and_indexed_clear() {
    const auto rom = program({
        0xA2, 0x01,       // LDX #1
        0x8A,             // TXA
        0x85, 0x10,       // STA $10
        0xC6, 0x10,       // DEC $10
        0x9E, 0x0F, 0x00, // STZ $000F,X -> $0010
        0xA5, 0x10,       // LDA $10
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 6; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "post-upload TXA, DEC dp, and STZ abs,X execute");
    check(cpu.registers().a == 0 &&
              (cpu.registers().p & 0x02U) != 0,
          "indexed clear and decrement preserve zero result");
}

void test_increment_accumulator_width() {
    const auto rom = program({
        0xA9, 0xFF, // LDA #$ff in emulation mode
        0x1A,       // INC A -> $00, Z=1
        0x18, 0xFB, // CLC; XCE
        0xC2, 0x20, // REP #$20
        0xA9, 0xFF, 0x00, // LDA #$00ff
        0x1A,       // INC A -> $0100, Z=0
    });
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    for (unsigned i = 0; i < 2; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "8-bit INC A executes");
    check((cpu.registers().a & 0xFFU) == 0 &&
              (cpu.registers().p & 0x02U) != 0,
          "8-bit INC A wraps and sets Z");
    for (unsigned i = 0; i < 5; ++i)
        check(cpu.step().error == sgb_test::Snes65c816TraceCpu::Error::none,
              "16-bit INC A setup and execute");
    check(cpu.registers().a == 0x0100 &&
              (cpu.registers().p & 0x02U) == 0,
          "16-bit INC A carries to high byte and clears Z");
}

void test_local_program(const std::filesystem::path& path, const bool sgb2) {
    const auto rom = gameboy::SgbProgramRom::from_file(path);
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    sgb_test::Snes65c816TraceCpu::StepResult result{};
    for (unsigned i = 0; i < 1000000 && cpu.apu_write_count() < 4; ++i) {
        result = cpu.step();
        if (result.error != sgb_test::Snes65c816TraceCpu::Error::none) break;
    }
    using Error = sgb_test::Snes65c816TraceCpu::Error;
    const auto first_step = sgb2 ? 28289U : 16628U;
    check(result.error == Error::none && cpu.steps() == first_step + 3 &&
              cpu.apu_write_count() == 4,
          "local program reaches its first four APU port clears");
    for (std::size_t port = 0; port < 4 && port < cpu.apu_write_count(); ++port) {
        const auto write = cpu.apu_write(port);
        check(write.step == first_step + port && write.port == port &&
                  write.value == 0 && apu.spc_read(0xF4 + port) == 0,
              "local port-clear sequence is mapped exactly");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3) {
        const auto mode = std::string_view(argv[1]);
        if (mode == "--local-sgb1" || mode == "--local-sgb2") {
            test_local_program(argv[2], mode == "--local-sgb2");
            return failures == 0 ? 0 : 1;
        }
    }
    if (argc != 1) return 2;
    test_native_width_and_apu_mapping();
    test_fail_closed();
    test_icd_wram_port_dma();
    test_host_math_results();
    test_lsr_direct_page();
    test_asl_direct_page();
    test_icd_source_contract();
    test_branch_carry_set();
    test_compare_y_immediate();
    test_compare_absolute_indexed_x();
    test_compare_absolute_indexed_y();
    test_test_and_set_direct();
    test_indexed_indirect_jump();
    test_ppu_counter_latch();
    test_subtract_absolute();
    test_indexed_indirect_subroutine();
    test_bit_immediate();
    test_add_absolute();
    test_decrement_absolute();
    test_load_x_direct();
    test_load_y_absolute_indexed_x();
    test_or_indirect_indexed_y();
    test_subtract_direct();
    test_and_direct();
    test_xor_direct();
    test_rotate_right_accumulator();
    test_increment_absolute_indexed_x();
    test_or_long_indirect_indexed_y();
    test_transfer_y_to_x();
    test_transfer_x_to_y();
    test_store_absolute_indexed_y();
    test_or_direct();
    test_hv_irq_latch();
    test_cli_clears_interrupt_disable();
    test_irq_entry_and_rti();
    test_push_x();
    test_push_y();
    test_pull_y();
    test_pull_x();
    test_subtract_immediate();
    test_ntsc_status_boundaries();
    test_status_latch_overscan_and_refresh();
    test_16_bit_direct_page_store();
    test_post_upload_arithmetic_and_indexed_clear();
    test_increment_accumulator_width();
    return failures == 0 ? 0 : 1;
}
