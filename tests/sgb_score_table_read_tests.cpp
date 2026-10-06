// SPDX-License-Identifier: GPL-3.0-or-later
#include "support/sgb_score_table_reads.hpp"
#include "gameboy/snes_spc700.hpp"
#include "gameboy/snes_audio_host.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void run(sgb_test::ScoreTableReads& reads, const gameboy::SnesApuBus::IplRom& image, unsigned steps) {
    gameboy::SnesApuBus bus;
    bus.install_ipl(image);
    bus.dsp_write_ram(0x2b00, 0x55);
    bus.dsp_write_ram(0x2b01, 0xaa);
    gameboy::SnesSpc700 cpu(bus);
    cpu.set_cycle_bus_enabled(true);
    cpu.set_bus_cycle_observer([](void* context, std::uint64_t cycle, char kind,
                                  std::uint16_t address, std::uint8_t) noexcept {
        static_cast<sgb_test::ScoreTableReads*>(context)->record(kind, cycle * 24, cycle * 2, address, 7);
    }, &reads);
    for (unsigned i = 0; i < steps; ++i)
        if (!cpu.step().supported) throw std::runtime_error("original fixture instruction failed");
}
}
int main() {
    try {
        sgb_test::ScoreTableReads empty, sample, overflow;
        // Independently authored MOV A,abs / MOV abs,A / BRA fixtures. Data
        // values are deliberately distinct; the observer never receives them.
        gameboy::SnesApuBus::IplRom image{};
        const unsigned char program[] = {0xe5,0x00,0x2b, 0xe5,0x01,0x2b,
            0xc5,0x00,0x2b, 0xe5,0x02,0x2b, 0xe5,0x05,0x2b,
            0xe5,0x06,0x2b, 0x2f,0xfe};
        for (unsigned i = 0; i < sizeof(program); ++i) image[i] = program[i];
        run(sample, image, 8);
        image = {};
        image[0] = 0xe5; image[1] = 0; image[2] = 0x2b;
        image[3] = 0x2f; image[4] = 0xfb;
        run(overflow, image, 600);
        std::cout << "{\"empty\":"; empty.write_json(std::cout);
        std::cout << ",\"sample\":"; sample.write_json(std::cout);
        std::cout << ",\"overflow\":"; overflow.write_json(std::cout);
        std::cout << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
