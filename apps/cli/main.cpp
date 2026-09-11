#include "gbb/core_registry.hpp"
#include "gbb/core_contract.hpp"
#include "gbb/frontend_logging.hpp"
#include "gbb/log.hpp"
#include "gbb/scene_json.hpp"

#include <cstdlib>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

struct MovieFrame {
    std::uint64_t frame{};
    std::uint8_t mask{};
};

constexpr std::array<gbb::InputId, 8> movie_inputs{
    gbb::InputId::right, gbb::InputId::left, gbb::InputId::up,
    gbb::InputId::down, gbb::InputId::a, gbb::InputId::b,
    gbb::InputId::select, gbb::InputId::start};

std::string_view input_name(const gbb::InputId input) {
    switch (input) {
    case gbb::InputId::right: return "right";
    case gbb::InputId::left: return "left";
    case gbb::InputId::up: return "up";
    case gbb::InputId::down: return "down";
    case gbb::InputId::a: return "a";
    case gbb::InputId::b: return "b";
    case gbb::InputId::select: return "select";
    case gbb::InputId::start: return "start";
    default: return "unknown";
    }
}

std::optional<std::size_t> input_index(const std::string_view name) {
    for (std::size_t index = 0; index < movie_inputs.size(); ++index) {
        if (input_name(movie_inputs[index]) == name) return index;
    }
    return {};
}

std::string movie_buttons(const std::uint8_t mask) {
    if (mask == 0) return "none";
    std::string result;
    for (std::size_t index = 0; index < movie_inputs.size(); ++index) {
        if ((mask & (std::uint8_t{1} << index)) == 0) continue;
        if (!result.empty()) result += '+';
        result += input_name(movie_inputs[index]);
    }
    return result;
}

std::vector<MovieFrame> read_input_movie(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Could not open input movie: " +
                                         path.string());

    std::vector<MovieFrame> frames;
    std::string line;
    std::size_t line_number = 0;
    bool header_seen = false;
    std::uint64_t previous_frame = 0;
    constexpr std::size_t maximum_keyframes = 1000000;
    while (std::getline(input, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream record(line);
        std::string first;
        if (!(record >> first)) continue;
        if (!header_seen) {
            if (first != "GBB_INPUT_MOVIE") {
                throw std::runtime_error(
                    "Input movie line " + std::to_string(line_number) +
                    " must begin with GBB_INPUT_MOVIE 1");
            }
            unsigned version = 0;
            if (!(record >> version) || version != 1) {
                throw std::runtime_error(
                    "Unsupported input movie version on line " +
                    std::to_string(line_number));
            }
            std::string extra;
            if (record >> extra) {
                throw std::runtime_error(
                    "Unexpected data after input movie header on line " +
                    std::to_string(line_number));
            }
            header_seen = true;
            continue;
        }

        if (frames.size() >= maximum_keyframes) {
            throw std::runtime_error("Input movie has too many keyframes");
        }
        std::uint64_t frame = 0;
        try {
            std::size_t consumed = 0;
            frame = std::stoull(first, &consumed, 10);
            if (consumed != first.size() || first.front() == '-') {
                throw std::invalid_argument("not an unsigned integer");
            }
        } catch (const std::exception&) {
            throw std::runtime_error("Invalid frame number on input movie line " +
                                     std::to_string(line_number));
        }
        if (!frames.empty() && frame <= previous_frame) {
            throw std::runtime_error(
                "Input movie frames must be strictly increasing (line " +
                std::to_string(line_number) + ")");
        }
        std::string buttons;
        if (!(record >> buttons)) {
            throw std::runtime_error("Missing buttons on input movie line " +
                                     std::to_string(line_number));
        }
        std::string extra;
        if (record >> extra) {
            throw std::runtime_error("Unexpected data on input movie line " +
                                     std::to_string(line_number));
        }
        std::uint8_t mask = 0;
        if (buttons != "none" && buttons != "-") {
            std::size_t start = 0;
            while (start < buttons.size()) {
                const auto separator = buttons.find('+', start);
                const auto token = std::string_view(
                    buttons).substr(start, separator == std::string::npos
                                             ? std::string::npos
                                             : separator - start);
                const auto index = input_index(token);
                if (!index) {
                    throw std::runtime_error(
                        "Unknown button '" + std::string(token) +
                        "' on input movie line " + std::to_string(line_number));
                }
                const auto bit = static_cast<std::uint8_t>(1U << *index);
                if ((mask & bit) != 0) {
                    throw std::runtime_error(
                        "Duplicate button on input movie line " +
                        std::to_string(line_number));
                }
                mask = static_cast<std::uint8_t>(mask | bit);
                if (separator == std::string::npos) break;
                start = separator + 1;
                if (start == buttons.size()) {
                    throw std::runtime_error(
                        "Empty button name on input movie line " +
                        std::to_string(line_number));
                }
            }
        }
        frames.push_back({frame, mask});
        previous_frame = frame;
    }
    if (!header_seen) {
        throw std::runtime_error("Input movie is missing the GBB_INPUT_MOVIE 1 header");
    }
    return frames;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: gbb_cli <rom> [instruction-count] "
                     "[--scene-json <output>] "
                     "[--scene-jsonl <output> --frames <count> "
                     "--input-movie <path> "
                     "[--max-instructions-per-frame <count>]]\n";
        return EXIT_FAILURE;
    }

    try {
        unsigned long instruction_count = 0;
        bool instruction_count_set = false;
        std::optional<std::filesystem::path> scene_json_path;
        std::optional<std::filesystem::path> scene_jsonl_path;
        std::optional<std::filesystem::path> input_movie_path;
        unsigned long observation_frames = 0;
        bool observation_frames_set = false;
        unsigned long max_instructions_per_frame = 200000;
        for (int index = 2; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--scene-json") {
                if (index + 1 >= argc) {
                    throw std::invalid_argument(
                        "--scene-json requires an output path");
                }
                scene_json_path = std::filesystem::path(argv[++index]);
            } else if (argument == "--scene-jsonl") {
                if (index + 1 >= argc) {
                    throw std::invalid_argument(
                        "--scene-jsonl requires an output path");
                }
                scene_jsonl_path = std::filesystem::path(argv[++index]);
            } else if (argument == "--frames") {
                if (index + 1 >= argc) {
                    throw std::invalid_argument("--frames requires a count");
                }
                observation_frames = std::stoul(argv[++index]);
                observation_frames_set = true;
            } else if (argument == "--input-movie") {
                if (index + 1 >= argc) {
                    throw std::invalid_argument(
                        "--input-movie requires a path");
                }
                input_movie_path = std::filesystem::path(argv[++index]);
            } else if (argument == "--max-instructions-per-frame") {
                if (index + 1 >= argc) {
                    throw std::invalid_argument(
                        "--max-instructions-per-frame requires a count");
                }
                max_instructions_per_frame = std::stoul(argv[++index]);
                if (max_instructions_per_frame == 0) {
                    throw std::invalid_argument(
                        "--max-instructions-per-frame must be positive");
                }
            } else if (!instruction_count_set) {
                instruction_count = std::stoul(argument);
                instruction_count_set = true;
            } else {
                throw std::invalid_argument("unknown command-line argument: " +
                                            argument);
            }
        }
        if (observation_frames_set && !scene_jsonl_path) {
            throw std::invalid_argument(
                "--frames requires --scene-jsonl");
        }
        if (scene_jsonl_path && !observation_frames_set) {
            throw std::invalid_argument(
                "--scene-jsonl requires --frames");
        }
        if (input_movie_path && !scene_jsonl_path) {
            throw std::invalid_argument(
                "--input-movie requires --scene-jsonl and --frames");
        }
        const auto input_movie = input_movie_path
                                    ? read_input_movie(*input_movie_path)
                                    : std::vector<MovieFrame>{};
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

        std::uint64_t total_cycles = 0;
        unsigned long executed_instructions = 0;
        for (unsigned long i = 0; i < instruction_count; ++i) {
            gbb::LogContextScope instruction_context{
                {0, static_cast<std::uint64_t>(i),
                 static_cast<std::uint64_t>(total_cycles),
                 core->rom_fingerprint()}};
            total_cycles += core->step_instruction();
            ++executed_instructions;
        }

        if (scene_jsonl_path) {
            if (scene_jsonl_path->empty()) {
                throw std::invalid_argument(
                    "--scene-jsonl requires a non-empty output path");
            }
            std::ofstream output(*scene_jsonl_path,
                                  std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error(
                    "Could not write scene observation log: " +
                    scene_jsonl_path->string());
            }

            std::uint8_t active_movie_mask = 0;
            std::uint8_t applied_movie_mask = 0;
            std::size_t next_movie_frame = 0;
            for (unsigned long frame = 0; frame < observation_frames; ++frame) {
                while (next_movie_frame < input_movie.size() &&
                       input_movie[next_movie_frame].frame <= frame) {
                    active_movie_mask = input_movie[next_movie_frame].mask;
                    ++next_movie_frame;
                }
                for (std::size_t button = 0; button < movie_inputs.size();
                     ++button) {
                    const auto bit = static_cast<std::uint8_t>(1U << button);
                    const auto wanted = (active_movie_mask & bit) != 0;
                    const auto applied = (applied_movie_mask & bit) != 0;
                    if (wanted != applied) {
                        core->set_input(movie_inputs[button], wanted);
                    }
                }
                applied_movie_mask = active_movie_mask;
                unsigned long frame_instructions = 0;
                while (!core->frame_ready()) {
                    total_cycles += core->step_instruction();
                    ++executed_instructions;
                    ++frame_instructions;
                    if (frame_instructions > max_instructions_per_frame) {
                        throw std::runtime_error(
                            "Core did not produce a frame while recording "
                            "scene observations");
                    }
                }

                auto snapshot = gbb::scene_snapshot_to_json(
                    core->scene_snapshot());
                if (!snapshot.empty() && snapshot.back() == '\n') {
                    snapshot.pop_back();
                }
                output << "{\"frame\":" << frame
                       << ",\"instructions\":" << executed_instructions
                       << ",\"rom_fingerprint\":"
                       << core->rom_fingerprint() << ",\"scene\":"
                       << snapshot << ",\"input_mask\":"
                       << static_cast<unsigned>(active_movie_mask)
                       << ",\"input_buttons\":\""
                       << movie_buttons(active_movie_mask) << "\"}\n";
                if (!output) {
                    throw std::runtime_error(
                        "Could not write scene observation log: " +
                        scene_jsonl_path->string());
                }
                core->consume_frame();
            }
            std::cout << "Scene observations: " << scene_jsonl_path->string()
                      << " (" << observation_frames << " frames)\n";
            if (input_movie_path) {
                std::cout << "Input movie: " << input_movie_path->string()
                          << " (" << input_movie.size() << " keyframes)\n";
            }
        }

        std::cout << "Executed " << std::dec << instruction_count
                  << " initial instructions (" << total_cycles << " total cycles)";
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
