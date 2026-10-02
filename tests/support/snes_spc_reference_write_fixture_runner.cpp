// Development-only adapter: external SPC700 sources are compiled from a
// user-supplied checkout, never linked into GBB or shipped with releases.
#include "snes_spc_write_fixture.hpp"
#include <processor/processor.hpp>
#include <processor/spc700/spc700.hpp>
#include <processor/spc700/spc700.cpp>

#include <array>

namespace {
struct Sink final : Processor::SPC700 {
    std::array<std::uint8_t, 65536> ram{};
    std::uint64_t cycles{};
    std::uint8_t opcode{};
    void idle() override { ++cycles; }
    uint8 read(uint16 address) override {
        ++cycles;
        // Synthetic fixtures never enable timers. Their counter overlays
        // therefore read zero, including the direct-page-zero wrap case.
        return address >= 0xfd && address <= 0xff ? 0 : ram[address];
    }
    void write(uint16 address, uint8 value) override {
        ++cycles;
        std::cout << "B " << cycles << ' ' << unsigned(opcode) << ' '
                  << unsigned(address) << ' ' << unsigned(value) << '\n';
        ram[address] = value;
        std::cout << "W " << cycles << ' ' << unsigned(opcode) << ' '
                  << unsigned(address) << ' ' << unsigned(value) << '\n';
    }
    bool synchronizing() const override { return true; }
    void load(std::uint16_t address, std::uint8_t value) { ram[address] = value; }
    bool run(unsigned count) {
        power();
        r.pc.w = 0xffc0; r.s = 0; r.p = 0;
        for (unsigned index = 0; index < count; ++index) {
            opcode = ram[r.pc.w];
            instruction();
        }
        std::cout << "E " << cycles << ' ' << unsigned(r.pc.w) << ' '
                  << unsigned(r.ya.byte.l) << ' ' << unsigned(r.x) << ' '
                  << unsigned(r.ya.byte.h) << ' ' << unsigned(r.s) << ' '
                  << unsigned(r.p) << '\n';
        return true;
    }
};
}

int main() { Sink sink; return sgb_test::run_spc_write_fixture(sink); }
