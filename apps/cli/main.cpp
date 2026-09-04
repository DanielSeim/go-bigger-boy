#include "gbb/core_registry.hpp"
#include "gbb/core_contract.hpp"
#include "gbb/frontend_logging.hpp"
#include "gbb/log.hpp"
#include "gbb/scene_json.hpp"

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
        std::string contract_error;
        if (!gbb::validate_core_contract(*core, contract_error)) {
            throw std::runtime_error("Core contract violation: " +
                                     contract_error);
        }
        const auto& descriptor = core->descriptor();
        gbb::LogContextScope log_context{
            {0, 0, 0, core->rom_fingerprint()}};
        std::cout << "Core: " << descriptor.core_name << '\n'
                  << "System: " << gbb::system_id_string(descriptor.system) << '\n'
                  << "Video: " << descriptor.video_width << 'x'
                  << descriptor.video_height << '\n';
        std::cout << "Title: " << descriptor.software_title << '\n'
                  << "ROM bytes: " << descriptor.rom_size << '\n'
                  << "RAM bytes: " << descriptor.save_ram_size << '\n'
                  << "Battery: " << (descriptor.has_battery ? "yes" : "no")
                  << '\n'
                  << "Color mode: "
                  << (descriptor.supports_color
                          ? (descriptor.requires_color ? "CGB only"
                                                        : "CGB enhanced")
                          : "DMG")
                  << '\n';

        unsigned long total_cycles = 0;
        for (unsigned long i = 0; i < instruction_count; ++i) {
            gbb::LogContextScope instruction_context{
                {0, static_cast<std::uint64_t>(i),
                 static_cast<std::uint64_t>(total_cycles),
                 core->rom_fingerprint()}};
            total_cycles += core->step_instruction();
        }

        std::cout << "Executed " << std::dec << instruction_count
                  << " instructions (" << total_cycles << " cycles)";
        if (const auto program_counter = core->program_counter()) {
            std::cout << ", PC=0x" << std::hex << std::setw(4)
                      << std::setfill('0') << *program_counter;
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
        gbb::log_frontend_error(std::string("CLI error: ") + error.what());
        return EXIT_FAILURE;
    }
}
