#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_spc700.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <system_error>
#include <string_view>

// Local-only diagnostic: reads a user-supplied S-SMP IPL image in place and
// reports the first interpreter gap. It never embeds or redistributes bytes.
int main(int argc, char** argv) {
    const bool event = argc == 3 && std::string_view(argv[2]) == "--upload-event";
    const bool upload = argc == 3 &&
        (std::string_view(argv[2]) == "--upload-smoke" || event);
    if (argc != 2 && !upload) {
        std::cerr << "usage: gameboy_snes_spc700_ipl_probe <64-byte-IPL-path> "
                     "[--upload-smoke|--upload-event]\n";
        return 2;
    }
    const std::filesystem::path path(argv[1]);
    std::error_code error;
    if (std::filesystem::file_size(path, error) != 64 || error) {
        std::cerr << "SPC700 IPL must be exactly 64 bytes\n";
        return 2;
    }
    std::ifstream input(path, std::ios::binary);
    gameboy::SnesApuBus::IplRom ipl{};
    if (!input.read(reinterpret_cast<char*>(ipl.data()), ipl.size())) {
        std::cerr << "could not read complete SPC700 IPL\n";
        return 2;
    }

    gameboy::SnesApuBus bus;
    bus.install_ipl(ipl);
    gameboy::SnesSpc700 cpu(bus);
    std::uint64_t cycles = 0;
    int fault = 0;
    const auto step_until = [&](auto condition, const unsigned limit) {
        for (unsigned step_count = 0; step_count < limit; ++step_count) {
            if (condition()) return true;
            const auto pc = cpu.registers().pc;
            const auto result = cpu.step();
            if (!result.supported) {
                std::cerr << "unsupported SPC700 opcode at PC $" << std::hex
                          << std::setw(4) << std::setfill('0') << pc << ": $"
                          << std::setw(2) << static_cast<unsigned>(result.opcode)
                          << std::dec << " after " << cycles << " cycles\n";
                fault = 3;
                return false;
            }
            cycles += result.cycles;
        }
        return condition();
    };
    if (!step_until([&] { return bus.host_read_port(0) == 0xAA &&
                                 bus.host_read_port(1) == 0xBB; }, 1000000)) {
        if (fault) return fault;
        std::cerr << "IPL did not reach ready handshake within 1000000 steps\n";
        return 4;
    }
    if (!event) {
        std::cout << "IPL ready handshake after " << cycles << " cycles\n";
    }
    if (!upload) return 0;

    // Original eight-byte test program: write KON=1 through DSPADDR/DSPDATA,
    // then loop. No SGB program or IPL bytes are embedded in this executable.
    constexpr std::array<std::uint8_t, 8> program{
        0x8F, 0x4C, 0xF2, 0x8F, 0x01, 0xF3, 0x2F, 0xFE,
    };
    constexpr std::uint16_t destination = 0x0200;
    bus.host_write_port(2, static_cast<std::uint8_t>(destination));
    bus.host_write_port(3, static_cast<std::uint8_t>(destination >> 8));
    bus.host_write_port(1, 1); // transfer command
    bus.host_write_port(0, 0xCC); // first kick
    if (!step_until([&] { return bus.host_read_port(0) == 0xCC; }, 10000)) {
        if (fault) return fault;
        std::cerr << "IPL did not acknowledge transfer kick\n";
        return 4;
    }
    for (unsigned index = 0; index < program.size(); ++index) {
        bus.host_write_port(1, program[index]);
        bus.host_write_port(0, static_cast<std::uint8_t>(index));
        if (!step_until([&] { return bus.host_read_port(0) == index; }, 10000)) {
            if (fault) return fault;
            std::cerr << "IPL did not acknowledge upload byte " << index << '\n';
            return 4;
        }
        // IPL acknowledges the byte before its subsequent RAM store.
        if (!step_until([&] { return bus.dsp_read_ram(
                static_cast<std::uint16_t>(destination + index)) == program[index]; },
                16)) {
            if (fault) return fault;
            std::cerr << "IPL acknowledged but did not store byte " << index << '\n';
            return 4;
        }
    }
    bus.host_write_port(2, static_cast<std::uint8_t>(destination));
    bus.host_write_port(3, static_cast<std::uint8_t>(destination >> 8));
    bus.host_write_port(1, 0); // entry command
    bus.host_write_port(0, static_cast<std::uint8_t>((program.size() + 2U) | 1U));
    if (!step_until([&] { return bus.dsp_register(0x4C) == 1; }, 10000)) {
        if (fault) return fault;
        std::cerr << "uploaded program did not reach DSP KON write\n";
        return 4;
    }
    if (event) {
        std::cout << cycles << " 76 1\n";
    } else {
        std::cout << "IPL upload and DSP KON write succeeded after " << cycles
                  << " cycles\n";
    }
    return 0;
}
