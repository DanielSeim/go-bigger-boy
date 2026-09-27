#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_spc700.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <system_error>

// Local-only diagnostic: reads a user-supplied S-SMP IPL image in place and
// reports the first interpreter gap. It never embeds or redistributes bytes.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: gameboy_snes_spc700_ipl_probe <64-byte-IPL-path>\n";
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
    for (unsigned step_count = 0; step_count < 1000000; ++step_count) {
        const auto pc = cpu.registers().pc;
        const auto result = cpu.step();
        if (!result.supported) {
            std::cerr << "unsupported SPC700 opcode at PC $" << std::hex
                      << std::setw(4) << std::setfill('0') << pc << ": $"
                      << std::setw(2) << static_cast<unsigned>(result.opcode)
                      << std::dec << " after " << cycles << " cycles\n";
            return 3;
        }
        cycles += result.cycles;
        if (bus.host_read_port(0) == 0xAA &&
            bus.host_read_port(1) == 0xBB) {
            std::cout << "IPL ready handshake after " << cycles << " cycles\n";
            return 0;
        }
    }
    std::cerr << "IPL did not reach ready handshake within 1000000 steps\n";
    return 4;
}
