#include "gameboy/cpu.hpp"
#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
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
    constexpr std::string_view title = "CPU EXECUTION";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    std::copy(program.begin(), program.end(), rom.begin() + program_address);
    return rom;
}

gameboy::CpuRegisters initial_registers() {
    return {
        0x77, 0xB0, 0x11, 0x22, 0x33, 0x44, 0xC0, 0x00, 0xFFFE,
        program_address,
    };
}

void test_sixteen_bit_operations() {
    struct PairLoadCase {
        std::uint8_t opcode;
        std::uint16_t expected;
    };
    constexpr std::array<PairLoadCase, 4> loads{{
        {0x01, 0xA1B2}, {0x11, 0xA1B2}, {0x21, 0xA1B2}, {0x31, 0xA1B2},
    }};
    for (unsigned index = 0; index < loads.size(); ++index) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({loads[index].opcode, 0xB2, 0xA1})}};
        gameboy::Cpu cpu;
        cpu.load_registers(initial_registers());
        check(cpu.step(bus) == 12, "LD rr,d16 takes twelve cycles");
        const auto& r = cpu.registers();
        const std::array<std::uint16_t, 4> actual{
            static_cast<std::uint16_t>((r.b << 8) | r.c),
            static_cast<std::uint16_t>((r.d << 8) | r.e),
            static_cast<std::uint16_t>((r.h << 8) | r.l), r.sp,
        };
        check(actual[index] == loads[index].expected,
              "LD rr,d16 loads the selected register pair");
        check(r.pc == 0x0103, "LD rr,d16 consumes three bytes");
    }

    gameboy::MemoryBus add_bus{gameboy::Cartridge{test_rom({0x09})}};
    auto add_registers = initial_registers();
    add_registers.b = 0x00;
    add_registers.c = 0x01;
    add_registers.h = 0xFF;
    add_registers.l = 0xFF;
    add_registers.f = 0x80;
    gameboy::Cpu add_cpu;
    add_cpu.load_registers(add_registers);
    check(add_cpu.step(add_bus) == 8, "ADD HL,rr takes eight cycles");
    check(add_cpu.registers().h == 0 && add_cpu.registers().l == 0,
          "ADD HL,rr wraps at sixteen bits");
    check(add_cpu.registers().f == 0xB0,
          "ADD HL,rr sets H/C, clears N, and preserves Z");

    gameboy::MemoryBus offset_bus{
        gameboy::Cartridge{test_rom({0xF8, 0xFF})}};
    auto offset_registers = initial_registers();
    offset_registers.sp = 0x0001;
    gameboy::Cpu offset_cpu;
    offset_cpu.load_registers(offset_registers);
    check(offset_cpu.step(offset_bus) == 12, "LD HL,SP+e8 takes twelve cycles");
    check(offset_cpu.registers().h == 0 && offset_cpu.registers().l == 0,
          "LD HL,SP+e8 sign-extends negative offsets");
    check(offset_cpu.registers().f == 0x30,
          "LD HL,SP+e8 derives H/C from the low unsigned byte addition");
}

void test_special_loads_and_decimal_adjust() {
    gameboy::MemoryBus store_bus{
        gameboy::Cartridge{test_rom({0x22, 0x3A})}};
    auto registers = initial_registers();
    registers.a = 0x42;
    gameboy::Cpu cpu;
    cpu.load_registers(registers);
    check(cpu.step(store_bus) == 8, "LD (HL+),A takes eight cycles");
    check(store_bus.read8(0xC000) == 0x42, "LD (HL+),A stores A");
    check(cpu.registers().l == 0x01, "LD (HL+),A increments HL");
    store_bus.write8(0xC001, 0x99);
    check(cpu.step(store_bus) == 8, "LD A,(HL-) takes eight cycles");
    check(cpu.registers().a == 0x99 && cpu.registers().l == 0x00,
          "LD A,(HL-) loads A and decrements HL");

    struct DaaCase {
        std::uint8_t a;
        std::uint8_t flags;
        std::uint8_t expected_a;
        std::uint8_t expected_flags;
    };
    constexpr std::array<DaaCase, 4> daa_cases{{
        {0x3C, 0x00, 0x42, 0x00},
        {0x32, 0x20, 0x38, 0x00},
        {0x9A, 0x00, 0x00, 0x90},
        {0x0F, 0x60, 0x09, 0x40},
    }};
    for (const auto& test : daa_cases) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({0x27})}};
        auto state = initial_registers();
        state.a = test.a;
        state.f = test.flags;
        gameboy::Cpu daa_cpu;
        daa_cpu.load_registers(state);
        check(daa_cpu.step(bus) == 4, "DAA takes four cycles");
        check(daa_cpu.registers().a == test.expected_a,
              "DAA produces the expected BCD result");
        check(daa_cpu.registers().f == test.expected_flags,
              "DAA produces the expected flags");
    }
}

void test_addressed_loads() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({
        0xEA, 0x00, 0xC1, // LD (C100),A
        0xFA, 0x00, 0xC2, // LD A,(C200)
        0xE0, 0x80,       // LDH (FF80),A
        0xF0, 0x81,       // LDH A,(FF81)
        0xE2,             // LD (FF00+C),A
        0xF2,             // LD A,(FF00+C)
        0x08, 0x00, 0xC3, // LD (C300),SP
    })}};
    bus.write8(0xC200, 0x55);
    bus.write8(0xFF81, 0x66);
    auto state = initial_registers();
    state.a = 0x42;
    state.c = 0x82;
    state.sp = 0xBEEF;
    gameboy::Cpu cpu;
    cpu.load_registers(state);

    check(cpu.step(bus) == 16 && bus.read8(0xC100) == 0x42,
          "LD (a16),A stores through an absolute address");
    check(cpu.step(bus) == 16 && cpu.registers().a == 0x55,
          "LD A,(a16) loads through an absolute address");
    check(cpu.step(bus) == 12 && bus.read8(0xFF80) == 0x55,
          "LDH (a8),A stores in high memory");
    check(cpu.step(bus) == 12 && cpu.registers().a == 0x66,
          "LDH A,(a8) loads from high memory");
    check(cpu.step(bus) == 8 && bus.read8(0xFF82) == 0x66,
          "LD (C),A uses C as a high-memory offset");
    bus.write8(0xFF82, 0x77);
    check(cpu.step(bus) == 8 && cpu.registers().a == 0x77,
          "LD A,(C) uses C as a high-memory offset");
    check(cpu.step(bus) == 20 && bus.read16(0xC300) == 0xBEEF,
          "LD (a16),SP stores little-endian SP");
    check(cpu.registers().pc == 0x010F,
          "addressed load sequence consumes every operand byte");

    gameboy::MemoryBus sp_bus{
        gameboy::Cartridge{test_rom({0xE8, 0xFF, 0xF9})}};
    auto sp_state = initial_registers();
    sp_state.sp = 0x0001;
    sp_state.h = 0xC1;
    sp_state.l = 0x23;
    gameboy::Cpu sp_cpu;
    sp_cpu.load_registers(sp_state);
    check(sp_cpu.step(sp_bus) == 16 && sp_cpu.registers().sp == 0,
          "ADD SP,e8 sign-extends a negative offset");
    check(sp_cpu.registers().f == 0x30,
          "ADD SP,e8 sets the expected low-byte carry flags");
    check(sp_cpu.step(sp_bus) == 8 && sp_cpu.registers().sp == 0xC123,
          "LD SP,HL copies the register pair");
}

void test_stack_load_table() {
    struct StackCase {
        std::uint8_t push_opcode;
        std::uint8_t pop_opcode;
        std::uint16_t value;
        std::uint16_t expected;
    };
    constexpr std::array<StackCase, 4> cases{{
        {0xC5, 0xC1, 0x1122, 0x1122},
        {0xD5, 0xD1, 0x3344, 0x3344},
        {0xE5, 0xE1, 0xC000, 0xC000},
        {0xF5, 0xF1, 0x77BF, 0x77B0},
    }};

    for (unsigned index = 0; index < cases.size(); ++index) {
        const auto& test = cases[index];
        gameboy::MemoryBus bus{gameboy::Cartridge{
            test_rom({test.push_opcode, test.pop_opcode})}};
        auto state = initial_registers();
        state.sp = 0xD000;
        gameboy::Cpu cpu;
        cpu.load_registers(state);

        check(cpu.step(bus) == 16, "PUSH qq takes sixteen cycles");
        check(cpu.registers().sp == 0xCFFE, "PUSH qq decrements SP by two");
        check(bus.read16(0xCFFE) ==
                  ((test.value & 0xFFF0U) |
                   (index == 3 ? 0U : (test.value & 0x000FU))),
              "PUSH qq writes the selected pair in little-endian order");

        bus.write16(0xCFFE, test.value);
        check(cpu.step(bus) == 12, "POP qq takes twelve cycles");
        check(cpu.registers().sp == 0xD000, "POP qq increments SP by two");
        const auto& result = cpu.registers();
        const std::array<std::uint16_t, 4> actual{
            static_cast<std::uint16_t>((result.b << 8) | result.c),
            static_cast<std::uint16_t>((result.d << 8) | result.e),
            static_cast<std::uint16_t>((result.h << 8) | result.l),
            static_cast<std::uint16_t>((result.a << 8) | result.f),
        };
        check(actual[index] == test.expected,
              "POP qq restores the selected pair and normalizes AF");
    }
}

void test_conditional_branch_tables() {
    struct ConditionCase {
        std::uint8_t jr_opcode;
        std::uint8_t jp_opcode;
        std::uint8_t flags_when_true;
        std::uint8_t flags_when_false;
    };
    constexpr std::array<ConditionCase, 4> conditions{{
        {0x20, 0xC2, 0x00, 0x80}, // NZ
        {0x28, 0xCA, 0x80, 0x00}, // Z
        {0x30, 0xD2, 0x00, 0x10}, // NC
        {0x38, 0xDA, 0x10, 0x00}, // C
    }};

    for (const auto& test : conditions) {
        for (const auto taken : {false, true}) {
            gameboy::MemoryBus jr_bus{gameboy::Cartridge{
                test_rom({test.jr_opcode, 0x02})}};
            auto state = initial_registers();
            state.f = taken ? test.flags_when_true : test.flags_when_false;
            gameboy::Cpu jr_cpu;
            jr_cpu.load_registers(state);
            check(jr_cpu.step(jr_bus) == (taken ? 12U : 8U),
                  "JR cc,e8 uses taken/not-taken timing");
            check(jr_cpu.registers().pc == (taken ? 0x0104 : 0x0102),
                  "JR cc,e8 applies the selected condition");

            gameboy::MemoryBus jp_bus{gameboy::Cartridge{
                test_rom({test.jp_opcode, 0x00, 0xC0})}};
            gameboy::Cpu jp_cpu;
            jp_cpu.load_registers(state);
            check(jp_cpu.step(jp_bus) == (taken ? 16U : 12U),
                  "JP cc,a16 uses taken/not-taken timing");
            check(jp_cpu.registers().pc == (taken ? 0xC000 : 0x0103),
                  "JP cc,a16 applies the selected condition");
        }
    }

    gameboy::MemoryBus relative_bus{
        gameboy::Cartridge{test_rom({0x18, 0xFE})}};
    gameboy::Cpu relative_cpu;
    relative_cpu.load_registers(initial_registers());
    check(relative_cpu.step(relative_bus) == 12 &&
              relative_cpu.registers().pc == 0x0100,
          "JR sign-extends negative offsets");

    gameboy::MemoryBus indirect_bus{
        gameboy::Cartridge{test_rom({0xE9})}};
    gameboy::Cpu indirect_cpu;
    indirect_cpu.load_registers(initial_registers());
    check(indirect_cpu.step(indirect_bus) == 4 &&
              indirect_cpu.registers().pc == 0xC000,
          "JP HL jumps without reading an immediate operand");
}

void test_calls_returns_and_restarts() {
    auto rom = test_rom({0xCD, 0x00, 0xC0});
    gameboy::MemoryBus bus{gameboy::Cartridge{std::move(rom)}};
    bus.write8(0xC000, 0xC9);
    auto state = initial_registers();
    state.sp = 0xD000;
    gameboy::Cpu cpu;
    cpu.load_registers(state);
    check(cpu.step(bus) == 24, "CALL a16 takes twenty-four cycles");
    check(cpu.registers().pc == 0xC000 && cpu.registers().sp == 0xCFFE,
          "CALL a16 jumps and reserves a stack word");
    check(bus.read16(0xCFFE) == 0x0103,
          "CALL a16 pushes the return address");
    check(cpu.step(bus) == 16 && cpu.registers().pc == 0x0103,
          "RET restores the return address");
    check(cpu.registers().sp == 0xD000, "RET releases its stack word");

    struct CallCase {
        std::uint8_t call_opcode;
        std::uint8_t return_opcode;
        std::uint8_t flags_when_true;
        std::uint8_t flags_when_false;
    };
    constexpr std::array<CallCase, 4> conditions{{
        {0xC4, 0xC0, 0x00, 0x80}, {0xCC, 0xC8, 0x80, 0x00},
        {0xD4, 0xD0, 0x00, 0x10}, {0xDC, 0xD8, 0x10, 0x00},
    }};
    for (const auto& test : conditions) {
        for (const auto taken : {false, true}) {
            gameboy::MemoryBus call_bus{gameboy::Cartridge{
                test_rom({test.call_opcode, 0x00, 0xC0})}};
            auto call_state = initial_registers();
            call_state.sp = 0xD000;
            call_state.f = taken ? test.flags_when_true : test.flags_when_false;
            gameboy::Cpu call_cpu;
            call_cpu.load_registers(call_state);
            check(call_cpu.step(call_bus) == (taken ? 24U : 12U),
                  "CALL cc,a16 uses taken/not-taken timing");
            check(call_cpu.registers().pc == (taken ? 0xC000 : 0x0103),
                  "CALL cc,a16 applies the selected condition");

            gameboy::MemoryBus return_bus{gameboy::Cartridge{
                test_rom({test.return_opcode})}};
            auto return_state = call_state;
            return_state.sp = 0xCFFE;
            return_bus.write16(0xCFFE, 0xC123);
            gameboy::Cpu return_cpu;
            return_cpu.load_registers(return_state);
            check(return_cpu.step(return_bus) == (taken ? 20U : 8U),
                  "RET cc uses taken/not-taken timing");
            check(return_cpu.registers().pc == (taken ? 0xC123 : 0x0101),
                  "RET cc applies the selected condition");
        }
    }

    constexpr std::array<std::uint8_t, 8> restarts{
        0xC7, 0xCF, 0xD7, 0xDF, 0xE7, 0xEF, 0xF7, 0xFF,
    };
    for (const auto opcode : restarts) {
        gameboy::MemoryBus rst_bus{
            gameboy::Cartridge{test_rom({opcode})}};
        auto rst_state = initial_registers();
        rst_state.sp = 0xD000;
        gameboy::Cpu rst_cpu;
        rst_cpu.load_registers(rst_state);
        check(rst_cpu.step(rst_bus) == 16, "RST vec takes sixteen cycles");
        check(rst_cpu.registers().pc == (opcode & 0x38),
              "RST vec selects the encoded vector");
        check(rst_bus.read16(0xCFFE) == 0x0101,
              "RST vec pushes the return address");
    }
}

void test_interrupt_and_low_power_states() {
    auto interrupt_rom = test_rom({0xFB, 0x00, 0x00});
    interrupt_rom[0x0040] = 0xD9; // RETI
    gameboy::MemoryBus interrupt_bus{
        gameboy::Cartridge{std::move(interrupt_rom)}};
    interrupt_bus.write8(0xFFFF, 0x01);
    interrupt_bus.write8(0xFF0F, 0x01);
    auto state = initial_registers();
    state.sp = 0xD000;
    gameboy::Cpu interrupt_cpu;
    interrupt_cpu.load_registers(state);

    check(interrupt_cpu.step(interrupt_bus) == 4 &&
              !interrupt_cpu.interrupts_enabled(),
          "EI does not enable interrupts immediately");
    check(interrupt_cpu.step(interrupt_bus) == 4 &&
              interrupt_cpu.interrupts_enabled(),
          "EI enables interrupts after the following instruction");
    check(interrupt_cpu.step(interrupt_bus) == 20,
          "interrupt dispatch takes twenty cycles");
    check(interrupt_cpu.registers().pc == 0x0040 &&
              interrupt_cpu.registers().sp == 0xCFFE,
          "interrupt dispatch pushes PC and selects the highest-priority vector");
    check(interrupt_bus.read16(0xCFFE) == 0x0102 &&
              (interrupt_bus.read8(0xFF0F) & 1) == 0,
          "interrupt dispatch stores PC and acknowledges IF");
    check(interrupt_cpu.step(interrupt_bus) == 16 &&
              interrupt_cpu.registers().pc == 0x0102 &&
              interrupt_cpu.interrupts_enabled(),
          "RETI restores PC and enables interrupts");
    check(interrupt_cpu.total_cycles() == 44,
          "CPU accumulates instruction, idle, and interrupt cycles");

    gameboy::MemoryBus repeated_ei_bus{
        gameboy::Cartridge{test_rom({0xFB, 0xFB, 0x00})}};
    repeated_ei_bus.write8(0xFFFF, 1);
    repeated_ei_bus.write8(0xFF0F, 1);
    gameboy::Cpu repeated_ei_cpu;
    repeated_ei_cpu.load_registers(initial_registers());
    static_cast<void>(repeated_ei_cpu.step(repeated_ei_bus));
    static_cast<void>(repeated_ei_cpu.step(repeated_ei_bus));
    check(repeated_ei_cpu.interrupts_enabled() &&
              repeated_ei_cpu.step(repeated_ei_bus) == 20,
          "repeating EI does not postpone an already scheduled IME enable");

    gameboy::MemoryBus interrupt_register_bus{
        gameboy::Cartridge{test_rom()}};
    interrupt_register_bus.write8(0xFF0F, 0x08);
    check(interrupt_register_bus.read8(0xFF0F) == 0xE8,
          "unused IF bits read high while writable interrupt flags are retained");

    gameboy::MemoryBus di_bus{
        gameboy::Cartridge{test_rom({0xFB, 0xF3, 0x00})}};
    di_bus.write8(0xFFFF, 1);
    di_bus.write8(0xFF0F, 1);
    gameboy::Cpu di_cpu;
    di_cpu.load_registers(initial_registers());
    static_cast<void>(di_cpu.step(di_bus));
    static_cast<void>(di_cpu.step(di_bus));
    check(!di_cpu.interrupts_enabled(), "DI cancels a pending EI enable");
    check(di_cpu.step(di_bus) == 4 && di_cpu.registers().pc == 0x0103,
          "DI prevents pending interrupts from dispatching");

    gameboy::MemoryBus halt_bus{
        gameboy::Cartridge{test_rom({0x76, 0x00})}};
    gameboy::Cpu halt_cpu;
    halt_cpu.load_registers(initial_registers());
    check(halt_cpu.step(halt_bus) == 4 && halt_cpu.halted(),
          "HALT enters the halted state");
    check(halt_cpu.step(halt_bus) == 4 && halt_cpu.registers().pc == 0x0101,
          "HALT idles without advancing PC");
    halt_bus.write8(0xFFFF, 1);
    halt_bus.write8(0xFF0F, 1);
    check(halt_cpu.step(halt_bus) == 4 && !halt_cpu.halted() &&
              halt_cpu.registers().pc == 0x0102,
          "a pending interrupt wakes HALT even when IME is clear");

    gameboy::MemoryBus bug_bus{
        gameboy::Cartridge{test_rom({0x76, 0x00, 0x00})}};
    bug_bus.write8(0xFFFF, 1);
    bug_bus.write8(0xFF0F, 1);
    gameboy::Cpu bug_cpu;
    bug_cpu.load_registers(initial_registers());
    static_cast<void>(bug_cpu.step(bug_bus));
    check(!bug_cpu.halted(), "HALT bug does not enter the halted state");
    static_cast<void>(bug_cpu.step(bug_bus));
    check(bug_cpu.registers().pc == 0x0101,
          "HALT bug suppresses the next opcode-fetch increment");
    static_cast<void>(bug_cpu.step(bug_bus));
    check(bug_cpu.registers().pc == 0x0102,
          "execution resumes normally after the HALT bug fetch");

    gameboy::MemoryBus stop_bus{
        gameboy::Cartridge{test_rom({0x10, 0x00, 0x00})}};
    gameboy::Cpu stop_cpu;
    stop_cpu.load_registers(initial_registers());
    check(stop_cpu.step(stop_bus) == 4 && stop_cpu.stopped() &&
              stop_cpu.registers().pc == 0x0102,
          "STOP consumes its padding byte and enters stopped state");
    check(stop_cpu.step(stop_bus) == 4 && stop_cpu.registers().pc == 0x0102,
          "STOP idles without advancing PC");
}

void test_base_rotate_table() {
    struct RotateCase {
        std::uint8_t opcode;
        std::uint8_t value;
        std::uint8_t flags;
        std::uint8_t expected;
        std::uint8_t expected_flags;
    };
    constexpr std::array<RotateCase, 4> cases{{
        {0x07, 0x80, 0xF0, 0x01, 0x10},
        {0x0F, 0x01, 0xF0, 0x80, 0x10},
        {0x17, 0x80, 0x10, 0x01, 0x10},
        {0x1F, 0x01, 0x10, 0x80, 0x10},
    }};
    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode})}};
        auto state = initial_registers();
        state.a = test.value;
        state.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(state);
        check(cpu.step(bus) == 4, "base accumulator rotate takes four cycles");
        check(cpu.registers().a == test.expected &&
                  cpu.registers().f == test.expected_flags,
              "base accumulator rotate produces the expected result and flags");
    }
}

void test_cb_rotate_shift_table() {
    struct CbCase {
        std::uint8_t opcode;
        std::uint8_t value;
        std::uint8_t flags;
        std::uint8_t expected;
        std::uint8_t expected_flags;
    };
    constexpr std::array<CbCase, 8> cases{{
        {0x00, 0x81, 0x00, 0x03, 0x10}, // RLC
        {0x08, 0x01, 0x00, 0x80, 0x10}, // RRC
        {0x10, 0x80, 0x10, 0x01, 0x10}, // RL
        {0x18, 0x01, 0x10, 0x80, 0x10}, // RR
        {0x20, 0x81, 0x00, 0x02, 0x10}, // SLA
        {0x28, 0x81, 0x00, 0xC0, 0x10}, // SRA
        {0x30, 0xF0, 0xF0, 0x0F, 0x00}, // SWAP
        {0x38, 0x01, 0x00, 0x00, 0x90}, // SRL
    }};

    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({0xCB, test.opcode})}};
        auto state = initial_registers();
        state.b = test.value;
        state.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(state);
        check(cpu.step(bus) == 8, "CB register rotate/shift takes eight cycles");
        check(cpu.registers().b == test.expected &&
                  cpu.registers().f == test.expected_flags,
              "CB register rotate/shift produces expected result and flags");

        const auto memory_opcode = static_cast<std::uint8_t>(test.opcode | 0x06);
        gameboy::MemoryBus memory_bus{
            gameboy::Cartridge{test_rom({0xCB, memory_opcode})}};
        memory_bus.write8(0xC000, test.value);
        gameboy::Cpu memory_cpu;
        memory_cpu.load_registers(state);
        check(memory_cpu.step(memory_bus) == 16,
              "CB (HL) rotate/shift takes sixteen cycles");
        check(memory_bus.read8(0xC000) == test.expected &&
                  memory_cpu.registers().f == test.expected_flags,
              "CB (HL) rotate/shift updates memory and flags");
    }
}

void test_cb_bit_reset_set_matrix() {
    for (unsigned bit = 0; bit < 8; ++bit) {
        const auto mask = static_cast<std::uint8_t>(1U << bit);
        for (const auto target : {0U, 6U}) {
            const auto bit_opcode = static_cast<std::uint8_t>(
                0x40 | (bit << 3) | target);
            gameboy::MemoryBus bit_bus{
                gameboy::Cartridge{test_rom({0xCB, bit_opcode})}};
            auto state = initial_registers();
            state.b = mask;
            state.f = 0x10;
            bit_bus.write8(0xC000, mask);
            gameboy::Cpu bit_cpu;
            bit_cpu.load_registers(state);
            check(bit_cpu.step(bit_bus) == (target == 6 ? 12U : 8U),
                  "BIT b,r uses register/(HL) timing");
            check(bit_cpu.registers().f == 0x30,
                  "BIT b,r preserves C, sets H, and reports a set bit");

            const auto res_opcode = static_cast<std::uint8_t>(
                0x80 | (bit << 3) | target);
            gameboy::MemoryBus res_bus{
                gameboy::Cartridge{test_rom({0xCB, res_opcode})}};
            res_bus.write8(0xC000, 0xFF);
            state.b = 0xFF;
            state.f = 0xB0;
            gameboy::Cpu res_cpu;
            res_cpu.load_registers(state);
            check(res_cpu.step(res_bus) == (target == 6 ? 16U : 8U),
                  "RES b,r uses register/(HL) timing");
            const auto res_value = target == 6 ? res_bus.read8(0xC000)
                                               : res_cpu.registers().b;
            check(res_value == static_cast<std::uint8_t>(0xFF & ~mask) &&
                      res_cpu.registers().f == 0xB0,
                  "RES b,r clears the selected bit and preserves flags");

            const auto set_opcode = static_cast<std::uint8_t>(
                0xC0 | (bit << 3) | target);
            gameboy::MemoryBus set_bus{
                gameboy::Cartridge{test_rom({0xCB, set_opcode})}};
            set_bus.write8(0xC000, 0x00);
            state.b = 0x00;
            gameboy::Cpu set_cpu;
            set_cpu.load_registers(state);
            check(set_cpu.step(set_bus) == (target == 6 ? 16U : 8U),
                  "SET b,r uses register/(HL) timing");
            const auto set_value = target == 6 ? set_bus.read8(0xC000)
                                               : set_cpu.registers().b;
            check(set_value == mask && set_cpu.registers().f == 0xB0,
                  "SET b,r sets the selected bit and preserves flags");
        }
    }
}

void test_base_opcode_completeness() {
    const auto invalid = [](const std::uint8_t opcode) {
        switch (opcode) {
        case 0xD3: case 0xDB: case 0xDD:
        case 0xE3: case 0xE4: case 0xEB: case 0xEC: case 0xED:
        case 0xF4: case 0xFC: case 0xFD:
            return true;
        default:
            return false;
        }
    };

    for (unsigned value = 0; value <= 0xFF; ++value) {
        const auto opcode = static_cast<std::uint8_t>(value);
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({opcode, 0x00, 0xC0})}};
        auto state = initial_registers();
        state.sp = 0xD000;
        gameboy::Cpu cpu;
        cpu.load_registers(state);
        auto rejected = false;
        try {
            static_cast<void>(cpu.step(bus));
        } catch (const gameboy::UnsupportedOpcode&) {
            rejected = true;
        }
        check(rejected == invalid(opcode),
              "base decoder accepts every legal opcode and rejects only invalid ones");
    }
}


} // namespace

int main() {
    test_sixteen_bit_operations();
    test_special_loads_and_decimal_adjust();
    test_addressed_loads();
    test_stack_load_table();
    test_conditional_branch_tables();
    test_calls_returns_and_restarts();
    test_interrupt_and_low_power_states();
    test_base_rotate_table();
    test_cb_rotate_shift_table();
    test_cb_bit_reset_set_matrix();
    test_base_opcode_completeness();
    return failures == 0 ? 0 : 1;
}
