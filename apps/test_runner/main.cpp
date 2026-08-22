#include "gameboy/emulator.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

enum class Protocol { automatic, mooneye, serial };

struct Options {
    std::string rom_path;
    std::uint64_t max_cycles = 100'000'000;
    Protocol protocol = Protocol::automatic;
};

void usage() {
    std::cerr << "Usage: gbb_test_runner <rom.gb> "
                 "[--max-cycles N] [--protocol auto|mooneye|serial]\n";
}

std::uint64_t parse_cycles(const std::string& text) {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed);
    if (consumed != text.size() || value == 0) {
        throw std::invalid_argument("max cycles must be a positive integer");
    }
    return value;
}

Options parse_options(const int argc, char** argv) {
    if (argc < 2) {
        throw std::invalid_argument("missing ROM path");
    }
    Options options;
    options.rom_path = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--max-cycles" && index + 1 < argc) {
            options.max_cycles = parse_cycles(argv[++index]);
        } else if (argument == "--protocol" && index + 1 < argc) {
            const std::string value = argv[++index];
            if (value == "auto") options.protocol = Protocol::automatic;
            else if (value == "mooneye") options.protocol = Protocol::mooneye;
            else if (value == "serial") options.protocol = Protocol::serial;
            else throw std::invalid_argument("unknown protocol: " + value);
        } else {
            throw std::invalid_argument("unknown or incomplete option: " + argument);
        }
    }
    return options;
}

bool mooneye_success(const gameboy::CpuRegisters& registers) {
    return registers.b == 3 && registers.c == 5 && registers.d == 8 &&
           registers.e == 13 && registers.h == 21 && registers.l == 34;
}

void print_state(const gameboy::Cpu& cpu) {
    const auto& r = cpu.registers();
    std::cerr << std::hex << std::setfill('0')
              << "PC=" << std::setw(4) << r.pc
              << " SP=" << std::setw(4) << r.sp
              << " AF=" << std::setw(2) << static_cast<unsigned>(r.a)
              << std::setw(2) << static_cast<unsigned>(r.f)
              << " BC=" << std::setw(2) << static_cast<unsigned>(r.b)
              << std::setw(2) << static_cast<unsigned>(r.c)
              << " DE=" << std::setw(2) << static_cast<unsigned>(r.d)
              << std::setw(2) << static_cast<unsigned>(r.e)
              << " HL=" << std::setw(2) << static_cast<unsigned>(r.h)
              << std::setw(2) << static_cast<unsigned>(r.l)
              << std::dec << " cycles=" << cpu.total_cycles() << '\n';
}

bool contains_failure(const std::string& output) {
    return output.find("Failed") != std::string::npos ||
           output.find("FAILED") != std::string::npos ||
           output.find("failed") != std::string::npos;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        auto emulator = gameboy::Emulator::from_file(options.rom_path);
        std::string serial_output;

        while (emulator.cpu().total_cycles() < options.max_cycles) {
            const auto& registers = emulator.cpu().registers();
            const bool watches_mooneye = options.protocol != Protocol::serial;
            if (watches_mooneye &&
                emulator.bus().read8(registers.pc) == 0x40) { // LD B,B
                if (mooneye_success(registers)) {
                    std::cout << "PASS (Mooneye)\n";
                    return EXIT_SUCCESS;
                }
                std::cerr << "FAIL (Mooneye result registers)\n";
                print_state(emulator.cpu());
                return EXIT_FAILURE;
            }

            static_cast<void>(emulator.step());
            auto bytes = emulator.bus().take_serial_output();
            if (!bytes.empty()) {
                serial_output += bytes;
                std::cout << bytes << std::flush;
            }

            const bool watches_serial = options.protocol != Protocol::mooneye;
            if (watches_serial && serial_output.find("Passed") != std::string::npos) {
                std::cout << "\nPASS (serial)\n";
                return EXIT_SUCCESS;
            }
            if (watches_serial && contains_failure(serial_output)) {
                std::cerr << "\nFAIL (serial)\n";
                print_state(emulator.cpu());
                return EXIT_FAILURE;
            }
        }

        std::cerr << "TIMEOUT after reaching the cycle limit\n";
        print_state(emulator.cpu());
        return 2;
    } catch (const std::exception& error) {
        usage();
        std::cerr << "Error: " << error.what() << '\n';
        return 2;
    }
}
