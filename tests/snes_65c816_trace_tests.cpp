#include "snes_65c816_trace_cpu.hpp"

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
    auto rom = program({0xAD, 0x12, 0x42}); // LDA $4212: unmodeled HVBJOY
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    const auto read = cpu.step();
    check(read.error == sgb_test::Snes65c816TraceCpu::Error::unsupported_read &&
              read.address == 0x004212 && cpu.apu_write_count() == 0,
          "unknown control-flow-relevant I/O reads trap");

    rom = program({0x02}); // COP is not silently treated as NOP
    sgb_test::Snes65c816TraceCpu unknown(rom, apu);
    const auto opcode = unknown.step();
    check(opcode.error == sgb_test::Snes65c816TraceCpu::Error::unsupported_opcode &&
              opcode.opcode == 0x02 && opcode.pc == 0x8104,
          "unsupported opcode reports its original PC");
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

void test_local_program(const std::filesystem::path& path, const bool sgb2) {
    const auto rom = gameboy::SgbProgramRom::from_file(path);
    gameboy::SnesApuBus apu;
    sgb_test::Snes65c816TraceCpu cpu(rom, apu);
    sgb_test::Snes65c816TraceCpu::StepResult result{};
    for (unsigned i = 0; i < 1000000; ++i) {
        result = cpu.step();
        if (result.error != sgb_test::Snes65c816TraceCpu::Error::none) break;
    }
    using Error = sgb_test::Snes65c816TraceCpu::Error;
    if (sgb2) {
        check(cpu.steps() == 24 && result.error == Error::unsupported_read &&
                  result.bank == 0x88 && result.pc == 0xC3E4 &&
                  result.address == 0x014212 && cpu.apu_write_count() == 0,
              "local SGB2 stops at dynamic HVBJOY before APU traffic");
    } else {
        check(cpu.steps() == 16635 && result.error == Error::unsupported_read &&
                  result.bank == 0 && result.pc == 0x831A &&
                  result.address == 0x01213F && cpu.apu_write_count() == 4,
              "local SGB1 clears four APU ports, then stops at PPU status");
        for (std::size_t port = 0; port < 4 && port < cpu.apu_write_count(); ++port) {
            const auto write = cpu.apu_write(port);
            check(write.step == 16628 + port && write.port == port &&
                      write.value == 0 && apu.spc_read(0xF4 + port) == 0,
                  "local SGB1 port-clear sequence is mapped exactly");
        }
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
    test_16_bit_direct_page_store();
    return failures == 0 ? 0 : 1;
}
