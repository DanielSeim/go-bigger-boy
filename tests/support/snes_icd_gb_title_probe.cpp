#include "snes_icd_gb_source.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: snes_icd_gb_title_probe GB-ROM GB-BOOT INPUT-SCRIPT\n";
        return 2;
    }
    try {
        sgb_test::SnesIcdGbSource source(
            argv[1], argv[2], gameboy::HardwareModel::sgb2);
        source.load_input_script(argv[3]);
        if (!source.write(0x6003, 0, 0x91))
            throw std::runtime_error("could not release the GB side");

        // SGB2 ICD divider 5, with roughly one GB frame per 70,224 cycles.
        // Packet reads happen at each interval, so the bounded ICD queue is
        // never treated as an unlimited recording buffer.
        constexpr std::uint64_t clocks_per_frame = 70224ULL * 5ULL;
        for (unsigned interval = 1; interval <= 4000; ++interval) {
            const auto clock = interval * clocks_per_frame;
            source.advance_to(clock);
            if (source.missing_address() != 0)
                throw std::runtime_error("GB ICD source data became unavailable");
            std::uint8_t available{};
            while (source.read(0x6002, clock, available) && available) {
                std::array<std::uint8_t, 16> packet{};
                for (unsigned byte = 0; byte < 16; ++byte) {
                    if (!source.read(static_cast<std::uint16_t>(0x7000 + byte),
                                     clock, packet[byte]))
                        throw std::runtime_error("GB ICD packet read failed");
                }
                if ((packet[0] >> 3) == 0x08 &&
                    source.audible_sound_commands() != 0 &&
                    packet == source.first_audible_sound_packet()) {
                    std::cout << "first audible SOUND frame="
                              << source.first_audible_frame() << " packet=";
                    for (const auto value : source.first_audible_sound_packet())
                        std::cout << ' ' << std::hex << std::setw(2)
                                  << std::setfill('0')
                                  << static_cast<unsigned>(value);
                    std::cout << std::dec
                              << " input_events=" << source.input_events_applied()
                              << " SOU_TRN=" << source.transfer_commands()
                              << " packets=" << source.packets_completed()
                              << '\n';
                    return 0;
                }
            }
            if (source.missing_address() != 0)
                throw std::runtime_error("GB ICD source data became unavailable");
        }
        std::cerr << "no audible SOUND within 4000 GB frame intervals\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "GB title probe failed: " << error.what() << '\n';
        return 2;
    }
}
