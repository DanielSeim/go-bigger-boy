// Development-only adapter: external SPC700 sources are compiled from a
// user-supplied checkout, never linked into GBB or shipped with releases.
#include "snes_spc_write_fixture.hpp"
#include <processor/processor.hpp>
#include <processor/spc700/spc700.hpp>
#include <processor/spc700/spc700.cpp>

#include <array>
#include <vector>

namespace {
struct Sink final : Processor::SPC700 {
    std::array<std::uint8_t, 65536> ram{};
    std::uint64_t cycles{};
    std::uint8_t opcode{};
    bool trace_bus{};
    std::array<unsigned, 3> timer_stage{};
    std::array<std::uint8_t, 3> timer_output{};
    std::array<std::uint8_t, 3> timer_target{};
    std::array<std::uint8_t, 4> ports{};
    std::array<std::uint8_t, 128> dsp{};
    std::uint8_t enabled{}, dsp_address{};
    std::vector<std::array<unsigned, 3>> host_events;
    void schedule_host(unsigned cycle, unsigned port, std::uint8_t value) {
        host_events.push_back({cycle, port, value});
    }
    void clock() {
        ++cycles;
        if (!trace_bus) return;
        // Independent one-clock analytical timer oracle, not the GBB bus.
        // Timer stages run on fixed 128/16-clock boundaries; output is 4-bit.
        for (unsigned index = 0; index < 3; ++index) {
            if (!(enabled & (1U << index)) || cycles % (index == 2 ? 16 : 128)) continue;
            timer_stage[index] = (timer_stage[index] + 1) & 255;
            if (timer_stage[index] == timer_target[index]) {
                timer_stage[index] = 0;
                timer_output[index] = (timer_output[index] + 1) & 15;
            }
        }
        for (const auto& event : host_events)
            if (event[0] == cycles) ports[event[1]] = event[2];
    }
    void idle() override {
        clock();
        if (trace_bus) std::cout << "I " << cycles << '\n';
    }
    uint8 read(uint16 address) override {
        clock();
        // Synthetic fixtures never enable timers. Their counter overlays
        // therefore read zero, including the direct-page-zero wrap case.
        auto value = address >= 0xfd && address <= 0xff ? 0 : ram[address];
        if (trace_bus) {
            if (address >= 0xfd && address <= 0xff) {
                value = timer_output[address - 0xfd];
                timer_output[address - 0xfd] = 0;
            } else if (address >= 0xf4 && address <= 0xf7) value = ports[address - 0xf4];
            else if (address == 0xf2) value = dsp_address;
            else if (address == 0xf3) value = dsp[dsp_address & 127];
            else if (address == 0xf0 || address == 0xf1 ||
                     (address >= 0xfa && address <= 0xfc)) value = 0;
        }
        if (trace_bus) std::cout << "R " << cycles << ' ' << unsigned(address)
                                 << ' ' << unsigned(value) << '\n';
        return value;
    }
    void write(uint16 address, uint8 value) override {
        clock();
        std::cout << "B " << cycles << ' ' << unsigned(opcode) << ' '
                  << unsigned(address) << ' ' << unsigned(value) << '\n';
        ram[address] = value;
        if (trace_bus) {
            if (address >= 0xfa && address <= 0xfc) timer_target[address - 0xfa] = value;
            else if (address == 0xf1) {
                for (unsigned index = 0; index < 3; ++index)
                    if ((value & (1U << index)) && !(enabled & (1U << index)))
                        timer_stage[index] = timer_output[index] = 0;
                enabled = value & 7;
                if (value & 16) ports[0] = ports[1] = 0;
                if (value & 32) ports[2] = ports[3] = 0;
            } else if (address == 0xf2) dsp_address = value;
            else if (address == 0xf3 && !(dsp_address & 128)) dsp[dsp_address] = value;
        }
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

int main(int argc, char** argv) {
    Sink sink;
    if (argc == 2 && std::string(argv[1]) == "--bus-cycles") sink.trace_bus = true;
    else if (argc != 1) return 2;
    return sgb_test::run_spc_write_fixture(sink);
}
