#include "gameboy/emulator.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

enum class Protocol { automatic, mooneye, serial, blargg };

struct Options {
    std::string rom_path;
    std::uint64_t max_cycles = 100'000'000;
    Protocol protocol = Protocol::automatic;
};

void usage() {
    std::cerr << "Usage: gbb_test_runner <rom.gb> "
                 "[--max-cycles N] [--protocol auto|mooneye|serial|blargg]\n";
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
            else if (value == "blargg") options.protocol = Protocol::blargg;
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

void print_recent_pcs(const std::array<std::uint16_t, 64>& pcs,
                      const std::size_t next, const std::size_t count) {
    std::cerr << "Recent PCs:" << std::hex << std::setfill('0');
    const auto begin = (next + pcs.size() - count) % pcs.size();
    for (std::size_t index = 0; index < count; ++index) {
        std::cerr << ' ' << std::setw(4) << pcs[(begin + index) % pcs.size()];
    }
    std::cerr << std::dec << '\n';
}

bool contains_failure(const std::string& output) {
    return output.find("Failed") != std::string::npos ||
           output.find("FAILED") != std::string::npos ||
           output.find("failed") != std::string::npos;
}

bool has_blargg_signature(const gameboy::MemoryBus& bus) {
    return bus.read8(0xA001) == 0xDE && bus.read8(0xA002) == 0xB0 &&
           bus.read8(0xA003) == 0x61;
}

std::string blargg_output(const gameboy::MemoryBus& bus) {
    std::string output;
    for (std::uint16_t address = 0xA004; address < 0xC000; ++address) {
        const auto value = bus.read8(address);
        if (value == 0) break;
        output.push_back(static_cast<char>(value));
    }
    return output;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        auto emulator = gameboy::Emulator::from_file(options.rom_path);
        std::string serial_output;
        std::string memory_output;
        auto saw_blargg = false;
        std::array<std::uint16_t, 64> recent_pcs{};
        std::size_t recent_pc_next = 0;
        std::size_t recent_pc_count = 0;
        std::uint16_t last_low_rom_pc = 0x0100;

        while (emulator.cpu().total_cycles() < options.max_cycles) {
            const bool watches_blargg = options.protocol == Protocol::automatic ||
                                         options.protocol == Protocol::blargg;
            if (watches_blargg && has_blargg_signature(emulator.bus())) {
                saw_blargg = true;
                const auto current_output = blargg_output(emulator.bus());
                if (current_output.size() > memory_output.size()) {
                    std::cout << current_output.substr(memory_output.size())
                              << std::flush;
                    memory_output = current_output;
                }
                const auto status = emulator.bus().read8(0xA000);
                if ((status == 0 &&
                     memory_output.find("Passed") != std::string::npos) ||
                    (status != 0 && status != 0x80 &&
                     contains_failure(memory_output))) {
                    if (status == 0) {
                        std::cout << "\nPASS (Blargg memory)\n";
                        return EXIT_SUCCESS;
                    }
                    std::cerr << "\nFAIL (Blargg result code "
                              << static_cast<unsigned>(status) << ")\n";
                    print_state(emulator.cpu());
                    print_recent_pcs(recent_pcs, recent_pc_next,
                                     recent_pc_count);
                    std::cerr << "Last low ROM PC=" << std::hex
                              << last_low_rom_pc << std::dec << '\n';
                    return EXIT_FAILURE;
                }
            }

            const auto& registers = emulator.cpu().registers();
            const bool watches_mooneye = options.protocol == Protocol::mooneye ||
                                         (options.protocol == Protocol::automatic &&
                                          !saw_blargg);
            if (watches_mooneye &&
                emulator.bus().read8(registers.pc) == 0x40) { // LD B,B
                const auto automatic_failure_signature =
                    registers.b == 0x42 && registers.c == 0x42 &&
                    registers.d == 0x42 && registers.e == 0x42 &&
                    registers.h == 0x42 && registers.l == 0x42;
                if (mooneye_success(registers)) {
                    std::cout << "PASS (Mooneye)\n";
                    return EXIT_SUCCESS;
                }
                if (options.protocol == Protocol::mooneye ||
                    automatic_failure_signature) {
                    std::cerr << "FAIL (Mooneye result registers)\n";
                    print_state(emulator.cpu());
                    print_recent_pcs(recent_pcs, recent_pc_next,
                                     recent_pc_count);
                    std::cerr << "Last low ROM PC=" << std::hex
                              << last_low_rom_pc << std::dec << '\n';
                    return EXIT_FAILURE;
                }
            }

            recent_pcs[recent_pc_next] = registers.pc;
            if (registers.pc < 0x4000) last_low_rom_pc = registers.pc;
            recent_pc_next = (recent_pc_next + 1) % recent_pcs.size();
            recent_pc_count = std::min(recent_pc_count + 1,
                                       recent_pcs.size());
            static_cast<void>(emulator.step());
            auto bytes = emulator.bus().take_serial_output();
            if (!bytes.empty()) {
                serial_output += bytes;
                std::cout << bytes << std::flush;
            }

            const bool watches_serial = options.protocol == Protocol::serial ||
                                        options.protocol == Protocol::automatic;
            if (watches_serial && serial_output.find("Passed") != std::string::npos) {
                std::cout << "\nPASS (serial)\n";
                return EXIT_SUCCESS;
            }
            if (watches_serial && contains_failure(serial_output)) {
                std::cerr << "\nFAIL (serial)\n";
                print_state(emulator.cpu());
                print_recent_pcs(recent_pcs, recent_pc_next, recent_pc_count);
                std::cerr << "Last low ROM PC=" << std::hex << last_low_rom_pc
                          << std::dec << '\n';
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
