#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_spc700.hpp"

#include <iostream>
#include <tuple>
#include <vector>

namespace {
using Event = std::tuple<std::uint64_t, char, std::uint16_t, std::uint8_t>;
void observe(void* opaque, std::uint64_t clock, char kind,
             std::uint16_t address, std::uint8_t value) noexcept {
    static_cast<std::vector<Event>*>(opaque)->emplace_back(clock,kind,address,value);
}
bool equal(const gameboy::SnesSpc700::Registers& a, const gameboy::SnesSpc700::Registers& b) {
    return a.pc==b.pc && a.a==b.a && a.x==b.x && a.y==b.y && a.sp==b.sp && a.psw==b.psw;
}
}
int main() {
    unsigned supported{};
    // Every opcode, both clock modes, multiple operands (including the input
    // ports' early-read addresses), and asynchronous host-port changes.
    for (bool half:{false,true}) for (unsigned opcode=0;opcode<256;++opcode)
    for (unsigned seed=0;seed<8;++seed) {
        gameboy::SnesApuBus a,b;
        gameboy::SnesSpc700 cached(a), reference(b);
        reference.debug_set_replay_cache_enabled(false);
        gameboy::SnesApuBus::IplRom program{};
        program[0]=opcode; program[1]=seed<4 ? 0xf4+seed : 0x13*seed;
        program[2]=seed&1 ? 0xff : 0x02;
        for (auto* bus:{&a,&b}) {
            bus->reset(); bus->install_ipl(program);
            for (unsigned address=0;address<0xffc0;++address)
                bus->dsp_write_ram(address,static_cast<std::uint8_t>(address*17+seed));
            bus->spc_write(0xf1,0x87); // IPL and timers enabled.
            bus->spc_write(0xfa,1); bus->spc_write(0xfb,3); bus->spc_write(0xfc,2);
        }
        cached.set_cycle_bus_enabled(true); reference.set_cycle_bus_enabled(true);
        std::vector<Event> x,y;
        cached.set_bus_cycle_observer(observe,&x); reference.set_bus_cycle_observer(observe,&y);
        cached.set_half_cycle_observer(observe,&x); reference.set_half_cycle_observer(observe,&y);
        for (unsigned clock=0;clock<32;++clock) {
            a.host_write_port(clock%4,clock*7); b.host_write_port(clock%4,clock*7);
            const auto p=half?cached.clock_half():cached.clock();
            const auto q=half?reference.clock_half():reference.clock();
            if (p.completed!=q.completed || p.instruction.supported!=q.instruction.supported ||
                p.instruction.cycles!=q.instruction.cycles || p.instruction.opcode!=q.instruction.opcode ||
                cached.half_cycles()!=reference.half_cycles() || x!=y ||
                !equal(cached.registers(),reference.registers())) {
                std::cerr << "SPC replay changed: opcode " << opcode << " seed " << seed
                          << " clock " << clock << " half " << half << '\n'; return 1;
            }
            if (p.completed) { supported+=p.instruction.supported; break; }
        }
        for (unsigned address=0;address<65536;++address)
            if (a.dsp_read_ram(address)!=b.dsp_read_ram(address)) return 1;
        for (unsigned port=0;port<4;++port) if (a.host_read_port(port)!=b.host_read_port(port)) return 1;
        for (unsigned address=0xf0;address<=0xff;++address)
            if (a.spc_read(address)!=b.spc_read(address)) return 1;
    }
    if (supported<1000) { std::cerr << "insufficient supported replay coverage\n"; return 1; }
    return 0;
}
