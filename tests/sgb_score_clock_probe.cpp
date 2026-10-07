// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/snes_apu_audio_engine.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Engine=gameboy::SnesApuAudioEngine;
void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
void clock(Engine& engine) {
    Engine::StereoSample sample;
    while (engine.pop_sample(sample))
        require(sample.left==0 && sample.right==0,"score clock must remain silent");
    require(engine.clock_half(),"native score clock stopped");
}
void setup(Engine& engine,const std::vector<unsigned char>& program,unsigned tempo) {
    gameboy::SnesApuBus::IplRom entry{};
    entry[0]=0x5f; entry[1]=0; entry[2]=8; // Owned JMP $0800 entry, no vendor IPL.
    engine.install_ipl(entry);
    engine.reset();
    for (unsigned i=0;i<program.size();++i) engine.bus().dsp_write_ram(0x0800+i,program[i]);
    engine.bus().dsp_write_ram(0x12,tempo);
}
void checkpoint(Engine& engine) {
    const auto saved=engine.save_state();
    Engine other;
    require(other.load_state(saved),"cross-instance score-clock restore rejected");
    for (unsigned i=0;i<4103;++i) { clock(engine); clock(other); }
    require(engine.save_state()==other.save_state(),"restored physical-half continuation differs");
    require(engine.load_state(saved),"score-clock rewind rejected");
    require(engine.save_state()==saved,"score-clock state roundtrip differs");
}
std::vector<std::uint64_t> exercise(Engine& engine,unsigned count,bool restore) {
    std::vector<std::uint64_t> ticks;
    unsigned char previous=0;
    for (unsigned halves=0;halves<4'000'000 && ticks.size()<count;++halves) {
        clock(engine);
        const auto low=engine.bus().dsp_read_ram(0x10);
        if (low!=previous) {
            require(low==static_cast<unsigned char>(previous+1),"score tick skipped or reversed");
            ticks.push_back(engine.cpu().half_cycles());
            previous=low;
        }
        if (restore && (halves==73 || halves==4097 || halves==10037 || halves==17005)) checkpoint(engine);
    }
    require(ticks.size()==count,"native score tick bound reached");
    require(engine.bus().dsp_read_ram(0x14)==1,"native clock not running");
    require(engine.bus().host_read_port(0)==0 && engine.bus().host_read_port(1)==0 &&
            engine.bus().host_read_port(2)==0 && engine.bus().host_read_port(3)==0,
            "experimental clock must not advertise mailbox readiness");
    require(engine.bus().dsp_register(0x6c)==0xe0,"experimental clock must retain DSP mute/reset");
    return ticks;
}
void write_ticks(const std::vector<std::uint64_t>& ticks) {
    std::cout<<'[';
    for (unsigned i=0;i<ticks.size();++i) { if (i) std::cout<<','; std::cout<<ticks[i]; }
    std::cout<<']';
}
void pending_pulses(const std::vector<unsigned char>& program,unsigned tempo) {
    Engine engine;
    setup(engine,program,tempo);
    unsigned halves=0;
    while (engine.bus().dsp_read_ram(0x14)!=1 && halves++<2000) clock(engine);
    require(engine.bus().dsp_read_ram(0x14)==1,"clock startup bound");
    // Synthetic timer accumulation: exercise count > 1 without claiming CPU stall timing.
    engine.bus().tick(3*2048);
    for (unsigned i=0;i<500;++i) clock(engine);
    require(engine.bus().dsp_read_ram(0x10)==3*tempo/256 &&
            engine.bus().dsp_read_ram(0x13)==(3*tempo)%256 &&
            engine.bus().dsp_read_ram(0x15)==0,"pending timer pulses lost");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"usage: score_clock_probe owned-program.bin");
        std::ifstream input(argv[1],std::ios::binary|std::ios::ate);
        require(input && input.tellg()>0 && input.tellg()<=512,"clock program must contain 1..512 bytes");
        std::vector<unsigned char> program(static_cast<std::size_t>(input.tellg()));
        input.seekg(0); require(bool(input.read(reinterpret_cast<char*>(program.data()),program.size())),"read clock program");
        std::cout<<"{\"schema\":\"gbb-spc-score-clock-v1\",\"qualification\":false,\"playback\":false,\"runs\":[";
        for (unsigned tempo:{96U,192U}) {
            Engine engine;
            setup(engine,program,tempo);
            const auto expected=exercise(engine,96,false);
            setup(engine,program,tempo);
            require(exercise(engine,96,true)==expected,"restore altered native tick timeline");
            setup(engine,program,tempo);
            require(exercise(engine,96,false)==expected,"cold reset altered native tick timeline");
            setup(engine,program,tempo);
            exercise(engine,300,false);
            require(engine.bus().dsp_read_ram(0x11)==1 && engine.bus().dsp_read_ram(0x10)==44,"16-bit score counter rollover");
            pending_pulses(program,tempo);
            if (tempo==192) std::cout<<',';
            std::cout<<"{\"tempo\":"<<tempo<<",\"tick_half_cycles\":"; write_ticks(expected);
            std::cout<<",\"reset_equal\":true,\"restore_equal\":true,\"rollover\":true,\"pending_pulses_equal\":true}";
        }
        for (unsigned tempo:{0U,95U,193U,255U}) {
            Engine engine; setup(engine,program,tempo);
            for (unsigned i=0;i<12000;++i) clock(engine);
            require(engine.bus().dsp_read_ram(0x14)==0xe1 && engine.bus().dsp_read_ram(0x10)==0,
                    "unsupported tempo must reject without score ticks");
        }
        std::cout<<"],\"unsupported_tempos_rejected\":true}\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
