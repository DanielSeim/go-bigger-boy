#include "snes_65c816_trace_cpu.hpp"

#include "gameboy/snes_spc700.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

// Original, test-only programs. No SGB firmware or IPL bytes are embedded.
gameboy::SgbProgramRom host_program() {
    std::vector<std::uint8_t> image(0x40000);
    image[0x7FD5] = 0x20;
    image[0x7FFC] = 0x04;
    image[0x7FFD] = 0x81;
    std::vector<std::uint8_t> code;
    for (unsigned loop = 0; loop < 5; ++loop) {
        code.insert(code.end(), {0xA2, 0x00, // LDX #0
                                 0xE8,       // INX
                                 0xD0, 0xFD}); // BNE INX
    }
    code.insert(code.end(), {
        0xA9, 0x4C, 0x8D, 0x41, 0x21, // DSP address: KON
        0xA9, 0x03, 0x8D, 0x42, 0x21, // two voices
        0xA9, 0xCC, 0x8D, 0x40, 0x21, // command last
        0x80, 0xFE,                   // wait indefinitely
    });
    for (std::size_t i = 0; i < code.size(); ++i) image[0x104 + i] = code[i];
    return gameboy::SgbProgramRom(std::move(image));
}

gameboy::SnesApuBus::IplRom audio_program() {
    gameboy::SnesApuBus::IplRom image{};
    constexpr std::array<std::uint8_t, 27> code{
        0x8F, 0xAA, 0xF4, // ready
        0x8F, 0xBB, 0xF5, // ready
        0x78, 0xCC, 0xF4, // poll command
        0xD0, 0xFB,
        0x8F, 0xCC, 0xF4, // acknowledge
        0xE4, 0xF5, 0xC4, 0xF2, // copy DSP address
        0xE4, 0xF6, 0xC4, 0xF3, // write DSP value
        0x8F, 0x00, 0xF4, // complete
        0x2F, 0xEB,       // poll again
    };
    for (std::size_t i = 0; i < code.size(); ++i) image[i] = code[i];
    return image;
}

struct Trace {
    unsigned pending{};
    std::uint8_t address{};
    std::uint8_t value{};
    std::uint64_t cycle{};
    unsigned writes{};
    bool invalid{};
};

void observe_write(void* context, std::uint8_t address,
                   std::uint8_t value) noexcept {
    auto& trace = *static_cast<Trace*>(context);
    ++trace.pending;
    trace.address = address;
    trace.value = value;
}

void observe_step(void* context, std::uint64_t cycle,
                  std::uint8_t opcode, unsigned) noexcept {
    auto& trace = *static_cast<Trace*>(context);
    if (trace.pending != 0) {
        if (trace.pending != 1 || opcode != 0xC4 || trace.writes != 0)
            trace.invalid = true;
        trace.cycle = cycle;
        ++trace.writes;
        trace.pending = 0;
    }
}

} // namespace

int main() {
    const auto rom = host_program();
    gameboy::SnesApuBus apu;
    apu.install_ipl(audio_program());
    gameboy::SnesSpc700 spc(apu);
    sgb_test::Snes65c816TraceCpu host(rom, apu, &spc);
    Trace trace;
    apu.set_dsp_write_observer(&observe_write, &trace);
    host.set_spc_step_observer(&observe_step, &trace);
    for (unsigned i = 0; i < 100000 && trace.writes == 0; ++i) {
        const auto result = host.step();
        if (result.error != sgb_test::Snes65c816TraceCpu::Error::none) {
            std::cerr << "synchronized SNES/SPC execution trapped at $" << std::hex
                      << result.pc << " opcode $" << static_cast<unsigned>(result.opcode)
                      << '\n';
            return 1;
        }
        host.clear_apu_writes();
    }
    apu.set_dsp_write_observer(nullptr);
    host.set_spc_step_observer(nullptr);
    if (trace.invalid || trace.writes != 1 || trace.address != 0x4C ||
        trace.value != 3 || trace.cycle < 2048 || trace.cycle >= 4096 ||
        apu.dsp_register(0x4C) != 3) {
        std::cerr << "bad synchronized DSP event: cycle=" << trace.cycle
                  << " writes=" << trace.writes << " address="
                  << static_cast<unsigned>(trace.address) << " value="
                  << static_cast<unsigned>(trace.value) << '\n';
        return 1;
    }
    std::cout << trace.cycle << ' ' << static_cast<unsigned>(trace.address)
              << ' ' << static_cast<unsigned>(trace.value) << '\n';
    return 0;
}
