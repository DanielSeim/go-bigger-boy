#include "gameboy/sgb_host.hpp"
#include <algorithm>
#include <iostream>
#include <limits>

namespace {
using Host=gameboy::SgbHost;
int failures{};
void check(bool value,const char* text) { if(!value) { std::cerr<<"FAIL: "<<text<<'\n'; ++failures; } }
gameboy::SgbHostConfig config(gameboy::HardwareModel model=gameboy::HardwareModel::sgb2) {
    gameboy::SgbHostConfig c;
    c.model=model;
    c.program_rom.resize(0x40000);
    c.program_rom[0x7fd5]=0x20; c.program_rom[0x7ffc]=4; c.program_rom[0x7ffd]=0x81;
    // Release GB, then mutate WRAM forever. No commercial code or firmware.
    const std::uint8_t snes[]{0x78,0xa9,0x81,0x8d,3,0x60,0xe6,0,0x80,0xfc};
    std::copy(std::begin(snes),std::end(snes),c.program_rom.begin()+0x104);
    c.game_rom.resize(32768); c.game_rom[0x146]=3;
    c.game_rom[0x100]=0xc3; c.game_rom[0x101]=0x50; c.game_rom[0x102]=1;
    c.game_rom[0x150]=0xc3; c.game_rom[0x151]=0x50; c.game_rom[0x152]=1;
    // Original minimal boot: LCD on, jump to our cartridge loop. No SGB packets.
    const std::uint8_t boot[]{0x3e,0x91,0xe0,0x40,0xc3,0,1};
    std::copy(std::begin(boot),std::end(boot),c.gb_boot_rom.begin());
    c.spc_ipl[0]=0x2f; c.spc_ipl[1]=0xfe;
    c.input_events={{0,0x80},{1,0},{2,0x10},{3,0}};
    return c;
}
std::vector<Host::StereoSample> drain(Host& h) {
    std::vector<Host::StereoSample> out; Host::StereoSample s;
    while(h.pop_sample(s)) out.push_back(s); return out;
}
bool equal(const std::vector<Host::StereoSample>& a,const std::vector<Host::StereoSample>& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),
        [](auto x,auto y){ return x.left==y.left && x.right==y.right; });
}
void checksum(std::vector<std::uint8_t>& state) {
    std::uint64_t h=14695981039346656037ULL;
    for(std::size_t n=0;n<state.size()-8;++n) { h^=state[n]; h*=1099511628211ULL; }
    for(unsigned n=0;n<8;++n) state[state.size()-8+n]=static_cast<std::uint8_t>(h>>(n*8));
}
void snapshots() {
    for(auto model:{gameboy::HardwareModel::sgb,gameboy::HardwareModel::sgb2}) {
        Host h(config(model)), other(config(model));
        const auto cold=h.save_state();
        check(h.load_state(cold),"cold host restores");
        for(unsigned boundary=0;boundary<32;++boundary) {
            const auto state=h.save_state();
            for(unsigned n=0;n<37;++n) check(h.step(),"synthetic instruction supported");
            const auto pcm=drain(h);
            const auto final=h.save_state();
            check(h.load_state(state),"same-instance whole-host restore");
            for(unsigned n=0;n<37;++n) check(h.step(),"restored instruction supported");
            check(equal(pcm,drain(h)) && h.save_state()==final,"whole-host continuation byte exact");
            check(other.load_state(state),"cross-instance whole-host restore");
            for(unsigned n=0;n<37;++n) check(other.step(),"cross-instance instruction supported");
            check(equal(pcm,drain(other)) && other.save_state()==final,"destination callbacks target restored owner");
        }
        const auto live=h.save_state();
        for(auto length:{std::size_t(0),std::size_t(8),live.size()-1}) {
            auto bad=live; bad.resize(length);
            check(!h.load_state(bad) && h.save_state()==live,"truncated restore is atomic");
        }
        auto bad=live; bad[8]=2;
        check(!h.load_state(bad) && h.save_state()==live,"unknown version rejected atomically");
        bad=live; bad[30]^=1;
        check(!h.load_state(bad) && h.save_state()==live,"corrupt payload rejected atomically");
        bad=live; bad.push_back(0);
        check(!h.load_state(bad) && h.save_state()==live,"trailing state rejected atomically");
        // Valid outer checksum, invalid inner ring index: exercise validation,
        // not merely the accidental-corruption guard.
        bad=live;
        constexpr auto head_offset=17+Host::buffer_capacity*16;
        bad[head_offset]=0; bad[head_offset+1]=0x40;
        checksum(bad);
        check(!h.load_state(bad) && h.save_state()==live,"invalid ring index rejected atomically after parsing");
        auto wrong=config(model); wrong.spc_ipl[63]=1; Host incompatible(std::move(wrong));
        check(!incompatible.load_state(live),"firmware identity bound to state");
        h.reset(); check(h.save_state()==cold,"full reset recreates exact cold state");
    }
}
void maximum_dma() {
    auto cfg=config();
    std::vector<std::uint8_t> code{0x78,0xa9,0x81,0x8d,3,0x60};
    for(unsigned ch=0;ch<8;++ch) {
        const std::uint8_t values[]{0,0x18,0,0,0x7e,0,0};
        for(unsigned off=0;off<7;++off)
            code.insert(code.end(),{0xa9,values[off],0x8d,static_cast<std::uint8_t>(ch*16+off),0x43});
    }
    const auto trigger_pc=0x8104+code.size()+5;
    code.insert(code.end(),{0xa9,0xff,0x8d,0x0b,0x42,0x18,0x80,0xfd});
    std::copy(code.begin(),code.end(),cfg.program_rom.begin()+0x104);
    Host h(std::move(cfg));
    while(h.cpu().registers().pc<trigger_pc) check(h.step(),"DMA setup supported");
    const auto pending=h.save_state();
    const auto before=h.samples_produced();
    const auto advanced=h.step();
    if(!advanced) std::cerr<<"DMA status="<<static_cast<unsigned>(h.status())<<" address="<<h.fault().address
                          <<" opcode="<<unsigned(h.fault().opcode)<<" pc="<<h.fault().pc<<'\n';
    check(advanced,"all eight maximum PPU DMA channels complete in one reservation");
    const auto samples=h.samples_produced()-before;
    check(samples>6000 && samples<Host::instruction_reserve,"maximum DMA output bounded below reserved headroom");
    const auto pcm=drain(h);
    const auto final=h.save_state();
    check(h.load_state(pending) && h.step(),"pending DMA state restores and runs once");
    check(equal(pcm,drain(h)) && h.save_state()==final,"DMA timing and PCM continuation exact");
}
void backpressure() {
    Host h(config()), ref(config());
    std::uint64_t steps{};
    while(h.step()) { ++steps; if(steps>1000000) break; }
    check(h.status()==Host::Status::buffer_full && h.pending_samples()>0,"instruction reservation applies bounded backpressure");
    const auto full=h.save_state(); const auto clock=h.cpu().timing().clocks();
    check(!h.step() && h.save_state()==full,"blocked retry repeats no host IO, GB or APU clocks");
    Host restored(config()); check(restored.load_state(full),"backpressured host restores");
    check(equal(drain(h),drain(restored)),"pending host PCM survives restore");
    for(std::uint64_t n=0;n<steps;++n) { check(ref.step(),"unbuffered reference advances"); (void)drain(ref); }
    check(h.save_state()==ref.save_state(),"draining pressure preserves exact complete state");
    check(h.run_until(clock,100)==0 && h.cpu().timing().clocks()==clock,"past target never rewinds or advances");
    check(h.run_until(clock+100000,0)==0,"zero instruction budget is no-op");
    check(h.run_until(clock+100000,100)==100,"instruction budget is honored");
    check(h.cpu().timing().clocks()>clock && h.apu_half_clocks()>0,"retry advances synchronized processors");
}
void faults() {
    auto c=config(); c.program_rom[0x104]=0xf8; Host host(std::move(c));
    check(!host.step() && host.status()==Host::Status::host_fault,"unsupported host opcode stops explicitly");
    const auto stopped=host.save_state();
    check(!host.step() && host.save_state()==stopped,"host fault cannot replay accesses");
    check(host.load_state(stopped) && !host.step(),"fault state remains terminal after restore");
    host.reset(); check(host.status()==Host::Status::ready,"reset clears terminal status");
    auto apu=config(); apu.spc_ipl[0]=0xff; Host af(std::move(apu));
    for(unsigned i=0;i<10 && af.step();++i) {}
    check(af.status()==Host::Status::apu_fault,"unsupported SPC instruction is reported separately");
    const auto astop=af.save_state(); check(af.load_state(astop) && !af.step(),"APU fault state restores without replay");
    auto icd=config(); icd.program_rom[0x10a]=0xad; icd.program_rom[0x10b]=8;
    icd.program_rom[0x10c]=0x60; Host cf(std::move(icd)); // LDA $6008 unsupported.
    for(unsigned i=0;i<10 && cf.step();++i) {}
    check(cf.status()==Host::Status::icd_fault,"unknown ICD write fails closed through host");
    bool rejected=false; try { auto wrong=config(); wrong.model=gameboy::HardwareModel::dmg; Host bad(std::move(wrong)); }
    catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"non-SGB model rejected");
}
}
int main() { snapshots(); backpressure(); maximum_dma(); faults(); return failures?1:0; }
