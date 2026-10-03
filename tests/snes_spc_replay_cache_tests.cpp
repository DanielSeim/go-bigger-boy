#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_spc700.hpp"
#include "gameboy/snes_apu_audio_engine.hpp"

#include <iostream>
#include <memory>
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
        reference.debug_set_idle_tail_cache_enabled(false);
        reference.debug_set_waiting_half_cache_enabled(false);
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
        for (unsigned clock=0;clock<128;++clock) {
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
            if (p.completed) {
                supported+=p.instruction.supported;
                if (!p.instruction.supported) break;
            }
        }
        for (unsigned address=0;address<65536;++address)
            if (a.dsp_read_ram(address)!=b.dsp_read_ram(address)) return 1;
        for (unsigned port=0;port<4;++port) if (a.host_read_port(port)!=b.host_read_port(port)) return 1;
        for (unsigned address=0xf0;address<=0xff;++address)
            if (a.spc_read(address)!=b.spc_read(address)) return 1;
    }
    if (supported<1000) { std::cerr << "insufficient supported replay coverage\n"; return 1; }
    // An opcode fetch can itself be an early input-port read. Exercise all
    // opcode bytes with PC=$00f4, not just operands that address the ports.
    for (unsigned opcode=0;opcode<256;++opcode) {
        gameboy::SnesApuBus a,b;
        gameboy::SnesSpc700 cached(a), reference(b);
        reference.debug_set_replay_cache_enabled(false);
        reference.debug_set_idle_tail_cache_enabled(false);
        reference.debug_set_waiting_half_cache_enabled(false);
        gameboy::SnesApuBus::IplRom program{};
        program[0]=0x5f; program[1]=0xf4; // JMP $00f4
        for(auto* bus:{&a,&b}) { bus->reset(); bus->install_ipl(program); }
        cached.set_cycle_bus_enabled(true); reference.set_cycle_bus_enabled(true);
        std::vector<Event> x,y;
        cached.set_half_cycle_observer(observe,&x); reference.set_half_cycle_observer(observe,&y);
        for(unsigned half=0;half<64;++half) {
            for(auto* bus:{&a,&b}) {
                bus->host_write_port(0,opcode);
                bus->host_write_port(1,half*11);
                bus->host_write_port(2,half*3);
                bus->host_write_port(3,half*5);
            }
            const auto p=cached.clock_half(), q=reference.clock_half();
            if(p.completed!=q.completed || p.instruction.supported!=q.instruction.supported ||
               p.instruction.cycles!=q.instruction.cycles || p.instruction.opcode!=q.instruction.opcode ||
               cached.half_cycles()!=reference.half_cycles() || x!=y ||
               !equal(cached.registers(),reference.registers())) {
                std::cerr << "early opcode fetch changed: " << opcode << '\n'; return 1;
            }
            if(p.completed && !p.instruction.supported) break;
        }
    }
    // Isolate the idle-tail cache: compare complete DSP/APU snapshots, not
    // just CPU registers, and invalidate the derived tail on restoration.
    for (unsigned opcode=0;opcode<256;++opcode) {
        gameboy::SnesApuBus a,b;
        gameboy::SnesSpc700 cached(a), reference(b);
        auto x=std::make_unique<gameboy::SnesApuAudioEngine>(cached);
        auto y=std::make_unique<gameboy::SnesApuAudioEngine>(reference);
        y->debug_set_direct_dsp_clock_enabled(false);
        reference.debug_set_replay_cache_enabled(false);
        reference.debug_set_idle_tail_cache_enabled(false);
        gameboy::SnesApuBus::IplRom program{};
        program[0]=opcode; program[1]=0xf4; program[2]=0x02;
        x->install_ipl(program); y->install_ipl(program);
        for(unsigned half=0;half<48;++half) {
            a.host_write_port(half%4,half*13);
            b.host_write_port(half%4,half*13);
            const auto p=x->clock_half(), q=y->clock_half();
            if(p!=q || x->save_state()!=y->save_state()) {
                std::cerr << "idle-tail snapshot changed: opcode " << opcode
                          << " half " << half << '\n'; return 1;
            }
            if(!p) break;
            if(half%11==7 && (!x->load_state(x->save_state()) ||
                              !y->load_state(y->save_state()))) return 1;
        }
    }
    // Absolute-load tails must not turn a first-half port latch into a later
    // re-read, collapse timer/DSP accesses, or survive a restored prefix.
    for (unsigned opcode:{0xE5U,0xE9U,0xECU,0xF5U,0xF6U})
    for (unsigned address:{0xF2U,0xF3U,0xF4U,0xF5U,0xF6U,0xF7U,
                           0xFDU,0xFEU,0xFFU,0x1234U,0xFFC0U,0xFFFFU})
    for (bool restore_state:{false,true})
    for (unsigned restore=0;restore<16;++restore) {
        auto a=std::make_unique<gameboy::SnesApuBus>();
        auto b=std::make_unique<gameboy::SnesApuBus>();
        gameboy::SnesSpc700 cached(*a), reference(*b);
        auto x=std::make_unique<gameboy::SnesApuAudioEngine>(cached);
        auto y=std::make_unique<gameboy::SnesApuAudioEngine>(reference);
        y->debug_set_direct_dsp_clock_enabled(false);
        reference.debug_set_replay_cache_enabled(false);
        reference.debug_set_idle_tail_cache_enabled(false);
        // Both sides use the same first-half placeholder representation;
        // its speculative counter differs from the fully uncached trace
        // oracle above, despite identical physical bus operations.
        gameboy::SnesApuBus::IplRom program{};
        program[0]=opcode;program[1]=address;program[2]=address>>8;
        program[3]=0x2f;program[4]=0xfb;
        x->install_ipl(program);y->install_ipl(program);
        a->spc_write(0xf1,0x87);b->spc_write(0xf1,0x87);
        for(unsigned half=0;half<48;++half) {
            for(unsigned port=0;port<4;++port) {
                a->host_write_port(port,half*13+port);
                b->host_write_port(port,half*13+port);
            }
            if(x->clock_half()!=y->clock_half() || x->save_state()!=y->save_state()) {
                std::cerr<<"absolute-load tail changed: opcode "<<opcode
                         <<" address "<<address<<" half "<<half<<'\n';return 1;
            }
            if(half==restore) {
                if(restore_state) {
                    if(!x->load_state(x->save_state()) ||
                       !y->load_state(y->save_state())) return 1;
                } else cached.debug_set_replay_cache_enabled(false);
            } else if(!restore_state && half==restore+1) {
                cached.debug_set_replay_cache_enabled(true);
            }
        }
    }
    // Taken and untaken branch tails with every relevant flag combination.
    for(unsigned opcode:{0x10U,0x30U,0x90U,0xB0U,0xD0U,0xF0U,0x2FU})
    for(unsigned value:{0U,1U,0x80U}) for(bool carry:{false,true})
    for(unsigned restore=0;restore<16;++restore) {
        auto a=std::make_unique<gameboy::SnesApuBus>();
        auto b=std::make_unique<gameboy::SnesApuBus>();
        gameboy::SnesSpc700 cached(*a),reference(*b);
        auto x=std::make_unique<gameboy::SnesApuAudioEngine>(cached);
        auto y=std::make_unique<gameboy::SnesApuAudioEngine>(reference);
        reference.debug_set_replay_cache_enabled(false);
        reference.debug_set_idle_tail_cache_enabled(false);
        y->debug_set_direct_dsp_clock_enabled(false);
        gameboy::SnesApuBus::IplRom program{};
        program[0]=0xe8;program[1]=value;program[2]=carry?0x80:0x60;
        program[3]=opcode;program[4]=0xfe;program[5]=0x2f;program[6]=0xfc;
        x->install_ipl(program);y->install_ipl(program);
        for(unsigned half=0;half<48;++half) {
            if(x->clock_half()!=y->clock_half() || x->save_state()!=y->save_state()) {
                std::cerr<<"branch operand tail changed: opcode "<<opcode
                         <<" value "<<value<<" carry "<<carry<<" half "<<half<<'\n';return 1;
            }
            if(half==restore && (!x->load_state(x->save_state()) ||
                                !y->load_state(y->save_state()))) return 1;
        }
    }
    return 0;
}
