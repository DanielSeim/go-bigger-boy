#include "snes_spc_write_fixture.hpp"
#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_spc700.hpp"

namespace {
struct Sink {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    gameboy::SnesSpc700 cpu{bus};
    bool valid{true};
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

int main() { Sink sink; return sgb_test::run_spc_write_fixture(sink); }
