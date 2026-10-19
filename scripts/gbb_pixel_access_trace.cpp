// SPDX-License-Identifier: GPL-3.0-or-later
// ROM BOOT [--end-only]; no callbacks or writes to emulated state.
#include "gameboy/emulator.hpp"
#include "pixel_access_trace.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static PixelAccessSample snapshot(const gameboy::Emulator& e, std::uint64_t origin) {
    const auto r=e.cpu().registers(); const auto& b=e.bus();
    const unsigned ly=b.read8(0xff44);
    return {b.debug_normalized_clocks()-origin,r.pc,
        static_cast<std::uint16_t>((r.a<<8)|r.f),static_cast<std::uint16_t>((r.b<<8)|r.c),
        static_cast<std::uint16_t>((r.d<<8)|r.e),static_cast<std::uint16_t>((r.h<<8)|r.l),r.sp,
        ly,static_cast<unsigned>(b.read8(0xff41)&3),b.debug_read_vram(0,0x100),
        b.debug_read_cgb_bg_palette(2),pixel_access_frontier(e.framebuffer().data(),ly)};
}
int main(int argc,char** argv) {
    if(argc<3 || argc>4 || (argc==4 && std::string(argv[3])!="--end-only")) return 2;
    try {
        std::ifstream input(argv[1],std::ios::binary),boot_file(argv[2],std::ios::binary);
        if(!input || !boot_file) return 2;
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input},{}};
        std::vector<std::uint8_t> boot{std::istreambuf_iterator<char>{boot_file},{}};
        if(bytes.size()!=32768 || bytes[0x147]!=0 || bytes[0x143]!=0xc0 || boot.size()!=256) return 2;
        gameboy::DiagnosticBootRom image{};std::copy(boot.begin(),boot.end(),image.begin());
        gameboy::Emulator e{gameboy::Cartridge{std::move(bytes)},gameboy::HardwareModel::cgb_e,
                           gameboy::BootRomMode::replacement};
        e.bus().install_boot_rom(image);
        PixelAccessSample samples[PIXEL_ACCESS_MAX_STEPS+1]{};
        unsigned count=0,steps=0,zeros=0;
        bool active=false,complete=false;const bool enabled=argc==3;
        std::uint64_t origin=0;
        while(e.bus().debug_normalized_clocks()<PIXEL_ACCESS_CLOCK_LIMIT) {
            const auto pc=e.cpu().registers().pc;
            if(!active && pc==0x1000) {active=true;origin=e.bus().debug_normalized_clocks();}
            if(active) {
                if(enabled) samples[count++]=snapshot(e,origin);
                if(pc==0x1100) {complete=true;break;}
                if(steps++==PIXEL_ACCESS_MAX_STEPS) break;
            }
            const auto before=e.bus().debug_normalized_clocks();(void)e.step();
            const auto after=e.bus().debug_normalized_clocks();
            if(after==before && ++zeros>1024) break;
            if(after>before) zeros=0;
            if(e.frame_ready()) {e.consume_frame();(void)e.take_audio_samples();}
        }
        const auto final=snapshot(e,origin);
        const auto state=e.save_state();
        pixel_access_print("gbb",complete,enabled,samples,count,final,state.size(),
                           pixel_access_hash(state.data(),state.size()));
        return complete?0:3;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"pixel access trace failed: %s\n",error.what());return 3;
    }
}
