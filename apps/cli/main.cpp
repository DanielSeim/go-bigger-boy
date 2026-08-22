#include "gameboy/emulator.hpp"

#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: gbb_cli <rom.gb> [instruction-count]\n";
        return EXIT_FAILURE;
    }

    try {
        auto emulator = gameboy::Emulator::from_file(argv[1]);
        const auto& cartridge = emulator.bus().cartridge();
        std::cout << "Title: " << cartridge.title() << '\n'
                  << "ROM bytes: " << cartridge.rom_size() << '\n'
                  << "RAM bytes: " << cartridge.ram_size() << '\n'
                  << "Battery: " << (cartridge.has_battery() ? "yes" : "no") << '\n'
                  << "Cartridge type: 0x" << std::hex << std::setw(2)
                  << std::setfill('0') << static_cast<unsigned>(cartridge.type()) << '\n';

        const auto instruction_count = argc == 3 ? std::stoul(argv[2]) : 0UL;
        unsigned long total_cycles = 0;
        for (unsigned long i = 0; i < instruction_count; ++i) {
            total_cycles += emulator.step();
        }

        const auto& registers = emulator.cpu().registers();
        std::cout << "Executed " << std::dec << instruction_count << " instructions ("
                  << total_cycles << " cycles), PC=0x" << std::hex << std::setw(4)
                  << std::setfill('0') << registers.pc << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
