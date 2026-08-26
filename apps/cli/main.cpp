#include "gbb/core_registry.hpp"
#include "gbb/gameboy_core.hpp"
#include "gameboy/emulator.hpp"

#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: gbb_cli <rom> [instruction-count]\n";
        return EXIT_FAILURE;
    }

    try {
        auto core = gbb::create_core_from_file(argv[1]);
        const auto& descriptor = core->descriptor();
        std::cout << "Core: " << descriptor.core_name << '\n'
                  << "System: " << gbb::system_id_string(descriptor.system) << '\n'
                  << "Video: " << descriptor.video_width << 'x'
                  << descriptor.video_height << '\n';
        const auto* emulator = gbb::gameboy_emulator(core.get());
        if (emulator != nullptr) {
            const auto& cartridge = emulator->bus().cartridge();
            std::cout << "Title: " << cartridge.title() << '\n'
                      << "ROM bytes: " << cartridge.rom_size() << '\n'
                      << "RAM bytes: " << cartridge.ram_size() << '\n'
                      << "Battery: "
                      << (cartridge.has_battery() ? "yes" : "no") << '\n'
                      << "Color mode: "
                      << (cartridge.supports_cgb()
                              ? (cartridge.requires_cgb() ? "CGB only"
                                                          : "CGB enhanced")
                              : "DMG")
                      << '\n'
                      << "Cartridge type: 0x" << std::hex << std::setw(2)
                      << std::setfill('0')
                      << static_cast<unsigned>(cartridge.type()) << '\n';
        }

        const auto instruction_count = argc == 3 ? std::stoul(argv[2]) : 0UL;
        unsigned long total_cycles = 0;
        for (unsigned long i = 0; i < instruction_count; ++i) {
            total_cycles += core->step_instruction();
        }

        std::cout << "Executed " << std::dec << instruction_count
                  << " instructions (" << total_cycles << " cycles)";
        if (emulator != nullptr) {
            std::cout << ", PC=0x" << std::hex << std::setw(4)
                      << std::setfill('0') << emulator->cpu().registers().pc;
        }
        std::cout << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
