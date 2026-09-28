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
    test_ntsc_status_boundaries();
    test_status_latch_overscan_and_refresh();
    test_16_bit_direct_page_store();
    return failures == 0 ? 0 : 1;
}
