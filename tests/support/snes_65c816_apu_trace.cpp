#include "snes_65c816_trace_cpu.hpp"
#include "gameboy/snes_spc700.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
    const bool trace = argc >= 3 && std::string_view(argv[argc - 1]) == "--trace";
    const bool upload = argc >= 3 && std::string_view(argv[argc - 1]) == "--upload";
    const bool upload_two = argc >= 3 &&
        std::string_view(argv[argc - 1]) == "--upload-two";
    const bool upload_three = argc >= 3 &&
        std::string_view(argv[argc - 1]) == "--upload-three";
    const bool upload_boot = argc >= 3 &&
        std::string_view(argv[argc - 1]) == "--upload-boot";
    const bool driver_probe = argc >= 3 &&
        std::string_view(argv[argc - 1]) == "--driver-probe";
    const int path_count = argc - 1 -
        (trace || upload || upload_two || upload_three || upload_boot || driver_probe ? 1 : 0);
    if (path_count < 1 || path_count > 2 ||
        ((upload || upload_two || upload_three || upload_boot || driver_probe) && path_count != 2)) {
        std::cerr << "usage: gameboy_snes_65c816_apu_trace <SGB-program-ROM> "
                     "[64-byte-SPC700-IPL] [--trace|--upload|--upload-two|"
                     "--upload-three|--upload-boot|--driver-probe]\n";
        return 2;
    }
    try {
        const auto rom = gameboy::SgbProgramRom::from_file(
            std::filesystem::path(argv[1]));
        gameboy::SnesApuBus apu;
        if (path_count == 2) {
            if (std::filesystem::file_size(argv[2]) != 64) {
                std::cerr << "SPC700 IPL must be exactly 64 bytes\n";
                return 2;
            }
            gameboy::SnesApuBus::IplRom image{};
            std::ifstream input(argv[2], std::ios::binary);
            if (!input.read(reinterpret_cast<char*>(image.data()), image.size())) {
                std::cerr << "could not read complete SPC700 IPL\n";
                return 2;
            }
            apu.install_ipl(image);
        }
        gameboy::SnesSpc700 spc(apu);
        sgb_test::Snes65c816TraceCpu cpu(rom, apu,
                                          path_count == 2 ? &spc : nullptr);
        std::size_t printed = 0;
        std::uint8_t ports[4]{};
        bool transfer_started = false;
        unsigned completed_blocks = 0;
        const unsigned requested_blocks = upload_boot || driver_probe ? 0U :
            upload_three ? 3U : upload_two ? 2U : 1U;
        bool entry_command_seen = false;
        std::uint16_t entry_address = 0;
        std::vector<std::pair<std::uint16_t, std::size_t>> uploaded_ranges;
        std::uint8_t expected_index = 0;
        std::uint16_t destination = 0;
        std::vector<std::uint8_t> transferred;
        for (unsigned i = 0; i < ((upload || upload_two || upload_three ||
                                      upload_boot || driver_probe) ?
                                        5000000U : 1000000U); ++i) {
            const auto result = cpu.step();
            if ((upload_boot || driver_probe) && entry_command_seen &&
                result.error == sgb_test::Snes65c816TraceCpu::Error::none &&
                spc.registers().pc < 0xFFC0) {
                std::cout << "SPC700 entered uploaded program at $" << std::hex
                          << std::setw(4) << std::setfill('0') << spc.registers().pc
                          << " (entry $" << std::setw(4) << entry_address
                          << ") after " << std::dec << cpu.steps()
                          << " SNES steps\n";
                if (driver_probe) {
                    struct FirstDspWrite {
                        bool seen{};
                        std::uint8_t address{};
                        std::uint8_t value{};
                    } first_dsp;
                    apu.set_dsp_write_observer(
                        [](void* context, std::uint8_t address,
                           std::uint8_t value) noexcept {
                            auto& first = *static_cast<FirstDspWrite*>(context);
                            if (!first.seen) {
                                first.seen = true;
                                first.address = address;
                                first.value = value;
                            }
                        }, &first_dsp);
                    for (unsigned step = 0; step < 100000 && !first_dsp.seen; ++step) {
                        const auto pc = spc.registers().pc;
                        const auto next = spc.step();
                        if (!next.supported) {
                            apu.set_dsp_write_observer(nullptr);
                            std::cerr << "SPC700 driver stopped at $" << std::hex
                                      << std::setw(4) << std::setfill('0') << pc
                                      << " unsupported opcode $" << std::setw(2)
                                      << static_cast<unsigned>(next.opcode)
                                      << std::dec << " after " << step
                                      << " driver instructions\n";
                            return 3;
                        }
                    }
                    apu.set_dsp_write_observer(nullptr);
                    if (!first_dsp.seen) {
                        std::cerr << "SPC700 driver reached instruction bound"
                                     " before first DSP write\n";
                        return 4;
                    }
                    std::cout << "first uploaded-driver DSP write: $" << std::hex
                              << std::setw(2) << std::setfill('0')
                              << static_cast<unsigned>(first_dsp.address)
                              << "=$" << std::setw(2)
                              << static_cast<unsigned>(first_dsp.value)
                              << std::dec << '\n';
                }
                return 0;
            }
            if (result.error != sgb_test::Snes65c816TraceCpu::Error::none) {
                std::cerr << "SNES CPU trace stopped after " << cpu.steps()
                          << " instructions at $" << std::hex << std::setw(2)
                          << std::setfill('0') << static_cast<unsigned>(result.bank)
                          << ':' << std::setw(4) << result.pc;
                if (result.error == sgb_test::Snes65c816TraceCpu::Error::unsupported_opcode) {
                    std::cerr << " unsupported opcode $" << std::setw(2)
                              << static_cast<unsigned>(result.opcode);
                } else if (result.error ==
                           sgb_test::Snes65c816TraceCpu::Error::unsupported_spc_opcode) {
                    std::cerr << " unsupported SPC700 opcode at $"
                              << std::setw(4) << result.address;
                } else {
                    std::cerr << " unsupported I/O/mapping access $"
                              << std::setw(6) << result.address;
                }
                std::cerr << std::dec << '\n';
                std::cerr << "master clocks " << cpu.timing().clocks()
                          << " V=" << cpu.timing().line()
                          << " H=" << cpu.timing().horizontal_clock() << '\n';
                return 3;
            }
            for (std::size_t event = 0; event < cpu.apu_write_count(); ++event) {
                const auto write = cpu.apu_write(event);
                ++printed;
                if (upload || upload_two || upload_three || upload_boot || driver_probe) {
                    ports[write.port] = write.value;
                    if (!transfer_started && write.port == 0 &&
                        write.value == 0xCC && ports[1] == 1) {
                        transfer_started = true;
                        destination = static_cast<std::uint16_t>(
                            ports[2] | (static_cast<unsigned>(ports[3]) << 8));
                    } else if (transfer_started && write.port == 0) {
                        if (write.value == expected_index) {
                            transferred.push_back(ports[1]);
                            ++expected_index;
                        } else if (write.value ==
                                   static_cast<std::uint8_t>(expected_index + 1)) {
                            if (transferred.empty()) {
                                std::cerr << "empty APU upload block\n";
                                return 5;
                            }
                            std::uint64_t hash = 14695981039346656037ULL;
                            for (std::size_t index = 0; index < transferred.size(); ++index) {
                                const auto actual = apu.dsp_read_ram(
                                    static_cast<std::uint16_t>(destination + index));
                                if (actual != transferred[index]) {
                                    std::cerr << "APU upload RAM mismatch at block "
                                              << completed_blocks + 1 << " byte " << index << '\n';
                                    return 5;
                                }
                                hash = (hash ^ actual) * 1099511628211ULL;
                            }
                            if (completed_blocks < 3) {
                                std::cout << (completed_blocks == 0 ? "first" :
                                              completed_blocks == 1 ? "second" : "third")
                                          << " APU upload block: ";
                            } else {
                                std::cout << "APU upload block " << completed_blocks + 1
                                          << ": ";
                            }
                            std::cout << "destination $" << std::hex
                                      << std::setw(4) << std::setfill('0') << destination
                                      << " bytes=" << std::dec << transferred.size()
                                      << " fnv64=" << std::hex << std::setw(16) << hash
                                      << std::dec << " SNES_steps=" << cpu.steps()
                                      << " master_clocks=" << cpu.timing().clocks()
                                      << " nmitimen=$" << std::hex
                                      << static_cast<unsigned>(cpu.interrupt_enable())
                                      << std::dec << " nmi_ever_enabled="
                                      << (cpu.nmi_was_enabled() ? 1 : 0)
                                      << " p=$" << std::hex
                                      << static_cast<unsigned>(cpu.registers().p)
                                      << std::dec << '\n';
                            uploaded_ranges.emplace_back(destination, transferred.size());
                            ++completed_blocks;
                            if (requested_blocks != 0 &&
                                completed_blocks == requested_blocks) return 0;
                            if ((upload_boot || driver_probe) && ports[1] == 0) {
                                entry_address = static_cast<std::uint16_t>(
                                    ports[2] | (static_cast<unsigned>(ports[3]) << 8));
                                bool entry_uploaded = false;
                                for (const auto& [start, length] : uploaded_ranges) {
                                    if (entry_address >= start &&
                                        static_cast<std::uint32_t>(entry_address) <
                                            static_cast<std::uint32_t>(start) + length) {
                                        entry_uploaded = true;
                                        break;
                                    }
                                }
                                if (!entry_uploaded) {
                                    std::cerr << "SPC700 entry is outside verified upload blocks\n";
                                    return 5;
                                }
                                entry_command_seen = true;
                                continue;
                            }
                            destination = static_cast<std::uint16_t>(
                                ports[2] | (static_cast<unsigned>(ports[3]) << 8));
                            transferred.clear();
                            expected_index = 0;
                        } else {
                            std::cerr << "unexpected APU upload sequence at byte "
                                      << transferred.size() << " expected index "
                                      << static_cast<unsigned>(expected_index)
                                      << " got " << static_cast<unsigned>(write.value)
                                      << " port1 " << static_cast<unsigned>(ports[1])
                                      << " PC $" << std::hex
                                      << static_cast<unsigned>(cpu.registers().pb)
                                      << ':' << cpu.registers().pc
                                      << " SPC $" << spc.registers().pc << std::dec << '\n';
                            return 5;
                        }
                    }
                    continue;
                }
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
            cpu.clear_apu_writes();
        }
        std::cerr << "SNES CPU trace reached instruction bound at $"
                  << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(cpu.registers().pb) << ':'
                  << std::setw(4) << cpu.registers().pc << std::dec
                  << " after " << cpu.timing().clocks() << " master clocks"
                  << " (V=" << cpu.timing().line()
                  << " H=" << cpu.timing().horizontal_clock() << ")\n";
        return 4;
    } catch (const std::exception& error) {
        std::cerr << "could not load SGB program ROM: " << error.what() << '\n';
        return 2;
    }
}
