#include "snes_65c816_trace_cpu.hpp"

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    const bool trace = argc == 3 && std::string_view(argv[2]) == "--trace";
    if (argc != 2 && !trace) {
        std::cerr << "usage: gameboy_snes_65c816_apu_trace <SGB-program-ROM> [--trace]\n";
        return 2;
    }
    try {
        const auto rom = gameboy::SgbProgramRom::from_file(
            std::filesystem::path(argv[1]));
        gameboy::SnesApuBus apu;
        sgb_test::Snes65c816TraceCpu cpu(rom, apu);
        std::size_t printed = 0;
        for (unsigned i = 0; i < 1000000; ++i) {
            const auto result = cpu.step();
            if (result.error != sgb_test::Snes65c816TraceCpu::Error::none) {
                std::cerr << "SNES CPU trace stopped after " << cpu.steps()
                          << " instructions at $" << std::hex << std::setw(2)
                          << std::setfill('0') << static_cast<unsigned>(result.bank)
                          << ':' << std::setw(4) << result.pc;
                if (result.error == sgb_test::Snes65c816TraceCpu::Error::unsupported_opcode) {
                    std::cerr << " unsupported opcode $" << std::setw(2)
                              << static_cast<unsigned>(result.opcode);
                } else {
                    std::cerr << " unsupported I/O/mapping access $"
                              << std::setw(6) << result.address;
                }
                std::cerr << std::dec << '\n';
                return 3;
            }
            while (printed < cpu.apu_write_count()) {
                const auto write = cpu.apu_write(printed++);
                if (trace) {
                    std::cout << write.step << ' ' << static_cast<unsigned>(write.port)
                              << ' ' << static_cast<unsigned>(write.value) << '\n';
                    if (printed == 64) return 0;
                } else {
                    std::cout << "first APU port write after " << write.step
                              << " instructions: port " << static_cast<unsigned>(write.port)
                              << " value $" << std::hex << std::setw(2)
                              << std::setfill('0') << static_cast<unsigned>(write.value)
                              << std::dec << '\n';
                    return 0;
                }
            }
        }
        std::cerr << "SNES CPU trace reached 1000000-instruction bound\n";
        return 4;
    } catch (const std::exception& error) {
        std::cerr << "could not load SGB program ROM: " << error.what() << '\n';
        return 2;
    }
}
