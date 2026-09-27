#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_spc700.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

struct DspWrite {
    std::uint8_t address{};
    std::uint8_t value{};
    unsigned count{};
};

struct ScheduledWrite {
    unsigned cycle{};
    std::uint8_t address{};
    std::uint8_t value{};
};

void observe_write(void* context, const std::uint8_t address,
                   const std::uint8_t value) noexcept {
    auto& write = *static_cast<DspWrite*>(context);
    write.address = address;
    write.value = value;
    ++write.count;
}

// This is an original synthetic program, not IPL or SGB firmware. It advertises
// readiness, polls a host command, then writes host-selected DSP register/data.
// C4 dp,A performs its write on its final (fourth) SPC700 cycle.
gameboy::SnesApuBus::IplRom command_program() {
    gameboy::SnesApuBus::IplRom image{};
    constexpr std::array<std::uint8_t, 27> code{
        0x8F, 0xAA, 0xF4, // MOV $F4,#$AA: ready
        0x8F, 0xBB, 0xF5, // MOV $F5,#$BB: ready
        0x78, 0xCC, 0xF4, // CMP $F4,#$CC
        0xD0, 0xFB,       // BNE polling
        0x8F, 0xCC, 0xF4, // MOV $F4,#$CC: command accepted
        0xE4, 0xF5,       // MOV A,$F5: DSP address
        0xC4, 0xF2,       // MOV $F2,A
        0xE4, 0xF6,       // MOV A,$F6: DSP value
        0xC4, 0xF3,       // MOV $F3,A
        0x8F, 0x00, 0xF4, // MOV $F4,#0: complete
        0x2F, 0xEB,       // BRA polling
    };
    static_assert(code.size() < image.size());
    for (std::size_t index = 0; index < code.size(); ++index) image[index] = code[index];
    return image;
}

bool run(std::array<ScheduledWrite, 2>& events) {
    gameboy::SnesApuBus bus;
    bus.install_ipl(command_program());
    gameboy::SnesSpc700 cpu(bus);
    DspWrite write;
    bus.set_dsp_write_observer(&observe_write, &write);
    unsigned cycles = 0;
    for (unsigned i = 0; i < 1000 && cycles < 2048; ++i) {
        const auto step = cpu.step();
        if (!step.supported || step.cycles == 0) return false;
        cycles += step.cycles;
        if (write.count != 0) return false; // No sound without host command.
    }
    if (cycles < 2048 || cycles > 2051 ||
        bus.host_read_port(0) != 0xAA || bus.host_read_port(1) != 0xBB ||
        bus.dsp_register(0x4C) != 0) return false;

    bus.host_write_port(0, 0xCC);
    bus.host_write_port(1, 0x4C); // KON
    bus.host_write_port(2, 0x03); // voices 0 and 1
    for (unsigned i = 0; i < 32 && write.count == 0; ++i) {
        const auto step = cpu.step();
        if (!step.supported || step.cycles == 0) return false;
        cycles += step.cycles;
        if (write.count != 0 && step.opcode != 0xC4) return false;
    }
    if (write.count != 1 || write.address != 0x4C || write.value != 0x03 ||
        bus.dsp_register(0x4C) != 0x03 || cycles >= 4096) return false;
    events[0] = {cycles, write.address, write.value};

    for (unsigned i = 0; i < 8 && bus.host_read_port(0) != 0; ++i) {
        const auto step = cpu.step();
        if (!step.supported || step.cycles == 0) return false;
        cycles += step.cycles;
    }
    if (bus.host_read_port(0) != 0 || write.count != 1) return false;
    // Clearing the host command must leave the program in its polling loop,
    // not repeatedly retrigger the same voice.
    bus.host_write_port(0, 0);
    for (unsigned i = 0; i < 1000 && cycles < 3072; ++i) {
        const auto step = cpu.step();
        if (!step.supported || step.cycles == 0) return false;
        cycles += step.cycles;
        if (write.count != 1) return false;
    }
    if (cycles < 3072 || cycles > 3075) return false;

    bus.host_write_port(0, 0xCC);
    bus.host_write_port(1, 0x5C); // KOFF
    bus.host_write_port(2, 0x03);
    for (unsigned i = 0; i < 32 && write.count == 1; ++i) {
        const auto step = cpu.step();
        if (!step.supported || step.cycles == 0) return false;
        cycles += step.cycles;
        if (write.count != 1 && step.opcode != 0xC4) return false;
    }
    if (write.count != 2 || write.address != 0x5C || write.value != 0x03 ||
        bus.dsp_register(0x5C) != 0x03 || cycles >= 4096) return false;
    events[1] = {cycles, write.address, write.value};
    for (unsigned i = 0; i < 8 && bus.host_read_port(0) != 0; ++i) {
        const auto step = cpu.step();
        if (!step.supported || step.cycles == 0) return false;
    }
    if (bus.host_read_port(0) != 0) return false;
    bus.host_write_port(0, 0);
    for (unsigned i = 0; i < 12; ++i) {
        const auto step = cpu.step();
        if (!step.supported || step.cycles == 0) return false;
    }
    return write.count == 2;
}

} // namespace

int main(int argc, char** argv) {
    std::array<ScheduledWrite, 2> events{};
    if (!run(events)) {
        std::cerr << "host-command sound handoff failed\n";
        return 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--event") {
        for (const auto& event : events) {
            std::cout << event.cycle << ' ' << static_cast<unsigned>(event.address)
                      << ' ' << static_cast<unsigned>(event.value) << '\n';
        }
    } else if (argc != 1) {
        return 2;
    }
    return 0;
}
