#include "snes_spc_write_fixture.hpp"
#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_spc700.hpp"

#include <array>
#include <vector>

namespace {
struct Sink {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    gameboy::SnesSpc700 cpu{bus};
    bool valid{true};
    std::vector<std::array<unsigned, 3>> host_events;
    void schedule_host(unsigned cycle, unsigned port, std::uint8_t value) {
        host_events.push_back({cycle, port, value});
    }
    void load(std::uint16_t address, std::uint8_t value) {
        bus.dsp_write_ram(address, value);
        if (address >= 0xffc0) ipl[address - 0xffc0] = value;
    }
    bool run(unsigned count) {
        bus.install_ipl(ipl);
        cpu.set_write_cycle_observer([](void* context, std::uint64_t cycle,
            std::uint8_t opcode, std::uint16_t address, std::uint8_t value, bool after) noexcept {
            auto& sink = *static_cast<Sink*>(context);
            if (cycle == 0) sink.valid = false;
            std::cout << (after ? "W " : "B ") << cycle << ' ' << unsigned(opcode)
                      << ' ' << address << ' ' << unsigned(value) << '\n';
        }, this);
        for (unsigned index = 0; index < count; ++index)
            if (!cpu.step().supported) return false;
        const auto& r = cpu.registers();
        std::cout << "E " << cpu.cycles() << ' ' << r.pc << ' ' << unsigned(r.a)
                  << ' ' << unsigned(r.x) << ' ' << unsigned(r.y) << ' '
                  << unsigned(r.sp) << ' ' << unsigned(r.psw) << '\n';
        cpu.set_write_cycle_observer(nullptr);
        return valid;
    }
};
}

int main(int argc, char** argv) {
    Sink sink;
    if (argc == 2 && std::string(argv[1]) == "--bus-cycles") {
        sink.cpu.set_cycle_bus_enabled(true);
        sink.cpu.set_bus_cycle_observer(
            [](void* context, std::uint64_t cycle, char kind, std::uint16_t address,
               std::uint8_t value) noexcept {
                auto& sink = *static_cast<Sink*>(context);
                if (kind == 'T') {
                    for (const auto& event : sink.host_events)
                        if (event[0] == cycle) sink.bus.host_write_port(event[1], event[2]);
                    return;
                }
                // Writes already have the matching before/after trace.
                if (kind == 'W') return;
                std::cout << kind << ' ' << cycle;
                if (kind == 'R') std::cout << ' ' << address << ' ' << unsigned(value);
                std::cout << '\n';
            }, &sink);
    } else if (argc != 1) return 2;
    return sgb_test::run_spc_write_fixture(sink);
}
