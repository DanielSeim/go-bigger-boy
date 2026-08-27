#include "gbb/core_registry.hpp"
#include "gbb/gameboy_core.hpp"
#include "gbb/scene_json.hpp"
#include "gameboy/emulator.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: gbb_cli <rom> [instruction-count] "
                     "[--scene-json <output>]\n";
        return EXIT_FAILURE;
    }

    try {
        unsigned long instruction_count = 0;
        bool instruction_count_set = false;
        std::optional<std::filesystem::path> scene_json_path;
        for (int index = 2; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--scene-json") {
                if (index + 1 >= argc) {
                    throw std::invalid_argument(
                        "--scene-json requires an output path");
                }
                scene_json_path = std::filesystem::path(argv[++index]);
            } else if (!instruction_count_set) {
                instruction_count = std::stoul(argument);
                instruction_count_set = true;
            } else {
                throw std::invalid_argument("unknown command-line argument: " +
                                            argument);
            }
        }
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
        if (scene_json_path) {
            if (!gbb::write_scene_snapshot_json(core->scene_snapshot(),
                                                *scene_json_path)) {
                throw std::runtime_error(
                    "Could not write scene snapshot: " +
                    scene_json_path->string());
            }
            std::cout << "Scene snapshot: " << scene_json_path->string()
                      << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
