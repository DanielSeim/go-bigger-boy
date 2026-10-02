#include "snes_icd_gb_source.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        for (const bool native : {false, true}) {
            sgb_test::SnesIcdGbSource source(argv[1], argv[2], gameboy::HardwareModel::sgb2);
            source.set_native_gb_input(native);
            source.load_input_script(argv[3]);
            const auto write = [&](unsigned address, std::uint64_t clock, unsigned value) {
                if (!source.write(address, clock, value)) throw std::runtime_error("controller write failed");
            };
            unsigned checkpoint = 0;
            const auto require = [&](bool value) {
                ++checkpoint;
                if (!value) throw std::runtime_error("ICD input checkpoint " + std::to_string(checkpoint) +
                    " native=" + std::to_string(native) + " JOYP=" +
                    std::to_string(source.diagnostic_joypad_read()));
            };
            write(0x6003, 0, 1);
            write(0x6003, 0, 0x81);
            source.advance_to(1000); // Original boot program selects actions and enables LCD.
            require((source.diagnostic_joypad_read() & 8) == 0); // Start at frame zero.
            write(0x6004, 1000, 0xff); // Reproduce the real overwrite path, not just a helper.
            require(((source.diagnostic_joypad_read() & 8) == 0) == native);
            write(0x6005, 1000, 0xff);
            source.advance_to(400000); // Frame one releases all buttons.
            require((source.diagnostic_joypad_read() & 0xf) == 0xf);
            source.advance_to(800000); // Frame two holds A.
            require((source.diagnostic_joypad_read() & 1) == 0);
            write(0x6004, 800000, 0xff);
            require(((source.diagnostic_joypad_read() & 1) == 0) == native);
            write(0x6003, 800000, 1);
            write(0x6003, 800000, 0x81);
            source.advance_to(801000);
            require((source.diagnostic_joypad_read() & 0xf) == 7); // Reset reapplies Start, not A.
            require(source.missing_address() == 0);
        }
        std::cout << "Actual ICD source reproduces legacy clobber and preserves native held input\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
