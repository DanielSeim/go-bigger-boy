// SPDX-License-Identifier: GPL-3.0-or-later
// ROM-free bootstrap contracts. No Nintendo artwork or instructions.
#include "gameboy/emulator.hpp"
#include "gameboy/sgb_icd_gb_source.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::vector<std::uint8_t> rom(unsigned pattern) {
    std::vector<std::uint8_t> bytes(32768);
    bytes[0x100] = 0x76;
    auto random=pattern;
    for (unsigned i=0x104; i<0x150; ++i) {
        random^=random<<13; random^=random>>17; random^=random<<5;
        bytes[i]=static_cast<std::uint8_t>(pattern<256 ? pattern*(i-0x104) : random);
    }
    bytes[0x143]=0; bytes[0x146]=3; bytes[0x147]=0; bytes[0x148]=0; bytes[0x149]=0;
    // Intentionally invalid checksum: GB-side SGB boot does not verify it.
    return bytes;
}
void finish(gameboy::Emulator& gb) {
    while (gb.bus().boot_rom_enabled() && gb.cpu().total_cycles()<5000000) {
        (void)gb.step();
        if (gb.frame_ready()) { (void)gb.take_audio_samples(); gb.consume_frame(); }
    }
    check(!gb.bus().boot_rom_enabled(),"SGB boot timed out");
}
void verify_wire(const std::vector<gameboy::MemoryBus::IoTraceEvent>& events) {
    std::uint64_t low_clock=0, high_clock=0, last_start=0;
    unsigned starts=0, bits=0;
    bool low=false;
    for(const auto& event:events) {
        if(event.address!=0xff00) continue;
        const auto value=event.value & 0x30;
        if(value==0x30) {
            if(low) check(event.cycle-low_clock==16,"bootstrap low pulse differs from measured four-M-cycle boot pulse");
            low=false; high_clock=event.cycle;
        } else {
            check(!low && event.cycle-high_clock>=(bits==128 ? 48 : 60),
                  "bootstrap high space differs from measured data/stop timing");
            low=true; low_clock=event.cycle;
            if(value==0) {
                if(starts) {
                    check(bits==129,"packet must contain 128 bits and a zero stop bit");
                    check(event.cycle-last_start>=4*70224-32768,"four-VBlank packet spacing");
                }
                last_start=event.cycle; ++starts; bits=0;
            } else {
                if(bits==128) check(value==0x20,"stop bit must be zero");
                ++bits;
            }
        }
    }
    check(starts==6 && bits==129 && !low,"complete six-packet wire trace");
}
std::vector<gameboy::MemoryBus::IoTraceEvent> timing_contract(const std::vector<std::uint8_t>& bytes) {
    // Execution-only observations, not an instruction trace. The initial LCD
    // line is 452 dots; subsequent frames are 70,224 dots. Polls are 32 apart.
    std::vector<gameboy::MemoryBus::IoTraceEvent> events{
        {32,0xff00,0x30},{229456,0xff26,0x80},{229464,0xff11,0x80},
        {229484,0xff12,0xf3},{229492,0xff25,0xf3},{229508,0xff24,0x77},
        {229528,0xff47,0xfc},{270848,0xff40,0x91}};
    std::uint64_t cycle=270884, last_vblank=0;
    const auto pulse=[&](std::uint8_t value) {
        events.push_back({cycle,0xff00,value}); cycle+=16;
        events.push_back({cycle,0xff00,0x30});
    };
    for(unsigned packet=0;packet<6;++packet) {
        std::array<std::uint8_t,16> payload{};
        payload[0]=static_cast<std::uint8_t>(0xf1+2*packet);
        for(unsigned i=0;i<14;++i) {
            const auto address=0x104+packet*14+i;
            payload[i+2]=address<0x150 ? bytes[address] : 0;
            payload[1]=static_cast<std::uint8_t>(payload[1]+payload[i+2]);
        }
        pulse(0);
        for(unsigned byte=0;byte<16;++byte) for(unsigned bit=0;bit<8;++bit) {
            const bool one=payload[byte] & (1U<<bit);
            cycle+=(bit ? 48 : byte ? 80 : 52)+(one ? 12 : 16);
            pulse(one ? 0x10 : 0x20);
        }
        cycle+=48; pulse(0x20);
        auto read=cycle+44;
        for(unsigned frame=0;frame<4;++frame) {
            auto edge=270848ULL+144*456-4;
            if(read>edge) edge+=((read-edge+70223)/70224)*70224;
            read+=((edge-read+31)/32)*32;
            last_vblank=read; read+=4144;
        }
        cycle=last_vblank+4184;
    }
    events.push_back({last_vblank+4188,0xff13,0xc1});
    events.push_back({last_vblank+4208,0xff14,0x07});
    events.push_back({last_vblank+4240,0xff50,1});
    return events;
}
void verify_timing(gameboy::Emulator& gb, const std::vector<std::uint8_t>& bytes,
                   const std::vector<gameboy::MemoryBus::IoTraceEvent>& actual) {
    auto expected=timing_contract(bytes);
    if(gb.hardware_model()==gameboy::HardwareModel::sgb2) expected.back().value=255;
    check(actual.size()==expected.size(),"boot timeline event count");
    for(unsigned i=0;i<expected.size();++i) {
        check(actual[i].cycle==expected[i].cycle && actual[i].address==expected[i].address &&
              actual[i].value==expected[i].value,"exact header-dependent boot IO timeline");
    }
    const auto clocks=expected.back().cycle;
    check(gb.cpu().total_cycles()==clocks && gb.bus().debug_divider_counter()==(clocks&65535),
          "complete divider phase and CPU handoff clock");
    check(gb.bus().debug_ppu_dot()==((clocks-270848+4)%70224)%456 &&
          gb.bus().debug_ppu_scanline()==153 && gb.bus().debug_ppu_mode()==1,"internal PPU handoff phase");
    std::array<unsigned,18> apu{};
    apu[0]=static_cast<unsigned>((clocks/8192-229456/8192)%8);
    apu[2]=static_cast<unsigned>(clocks*48000%gameboy::hardware_clock_rate_hz(gb.hardware_model()));
    apu[5]=64; apu[17]=32767;
    check(gb.bus().debug_apu_clock_state()==apu,"APU sequencer/channel/resampler handoff phases");
}
void verify_icd_scanline(gameboy::HardwareModel model) {
    using namespace gameboy;
    SgbIcdGbSource source(rom(0),model==HardwareModel::sgb ? sgb_boot_rom() : sgb2_boot_rom(),model);
    source.set_native_gb_input(true);
    check(source.write(0x6003,0,1) && source.write(0x6003,0,0x81),"ICD reset/release");
    const auto host_clock=[&](std::uint64_t cycles) {
        return model==HardwareModel::sgb ? cycles*5 : (cycles*21477273+4194303)/4194304;
    };
    std::uint8_t status=0;
    check(source.read(0x6000,host_clock(270848+153*456-4+24),status),"ICD final-line read");
    check(source.emulator().bus().read8(0xff44)==0 &&
          source.emulator().bus().debug_ppu_scanline()==153 && status==0x8b,
          "early CPU LY zero must not publish visible-row ICD status during VBlank");
    check(source.read(0x6000,host_clock(270848+154*456-4+24),status) && status==0,
          "ICD visible row zero starts only at the physical frame boundary");
}
void verify(gameboy::Emulator& gb, const std::vector<std::uint8_t>& bytes, unsigned first_packet=0) {
    using namespace gameboy;
    const auto& r=gb.cpu().registers();
    check(r.a==(gb.hardware_model()==HardwareModel::sgb2 ? 255 : 1) && r.f==0 &&
        r.b==0 && r.c==20 && r.d==0 && r.e==0 && r.h==0xc0 && r.l==0x60 &&
        r.sp==0xfffe && r.pc==0x100 && !gb.cpu().interrupts_enabled(),"SGB handoff registers");
    const auto& diag=gb.bus().debug_sgb_diagnostics();
    check(diag.enabled && diag.packets_completed==6-first_packet && diag.malformed_packets==0,
          "all six independent header packets parsed");
    const auto& adapter=gb.bus().debug_sgb_adapter();
    check(adapter.command_history_size()==6-first_packet,"bootstrap history has expected packets");
    for(unsigned p=0;p<6;++p) {
        std::array<std::uint8_t,16> expected{};
        expected[0]=static_cast<std::uint8_t>(0xf1+2*p);
        for(unsigned b=0;b<14;++b) {
            const auto addr=0x104+14*p+b;
            expected[2+b]=addr<0x150 ? bytes[addr] : 0;
            expected[1]=static_cast<std::uint8_t>(expected[1]+expected[2+b]);
        }
        if (p>=first_packet) {
            const auto& record=adapter.command_history()[p-first_packet];
            check(record.packet_bytes==16 && std::equal(expected.begin(),expected.end(),record.packet.begin()),
                  "header payload/sum/padding differs");
        }
        for(unsigned b=0;b<16;++b)
            check(gb.bus().read8(static_cast<std::uint16_t>(0xc000+16*p+b))==expected[b],"bootstrap RAM differs");
    }
    check(gb.bus().read8(0xff44)==0 && gb.bus().read8(0xff40)==0x91 &&
          gb.bus().read8(0xff00)==0xff && gb.bus().read8(0xffff)==0,"SGB LCD/JOYP/interrupt handoff");
    check(!gb.startup_animation_active(),"SGB must not show DMG intro");
}
}
int main() {
    try {
        using namespace gameboy;
        check(sgb_boot_rom().size()==256 && sgb2_boot_rom().size()==256,"256-byte images");
        verify_icd_scanline(HardwareModel::sgb); verify_icd_scanline(HardwareModel::sgb2);
        for(auto model:{HardwareModel::sgb,HardwareModel::sgb2})
        for(auto mode:{BootRomMode::replacement_sgb,BootRomMode::animated_dmg})
        for(unsigned pattern:{0U,1U,255U,256U,257U,258U,259U,260U,261U,262U,263U,
                              264U,265U,266U,267U,268U,269U,270U,271U}) {
            auto bytes=rom(pattern);
            Emulator gb(Cartridge(bytes),model,mode);
            check(gb.cpu().registers().pc==0 && gb.bus().boot_rom_enabled(),"cold reset mapping");
            gb.bus().debug_enable_io_trace(true);
            while(gb.bus().debug_sgb_diagnostics().packets_completed<2) (void)gb.step();
            // Restore during a live packet, not merely during the waiting loop.
            while((gb.bus().read8(0xff00)&0x30)!=0 && gb.cpu().total_cycles()<5000000) (void)gb.step();
            for(unsigned i=0;i<50;++i) (void)gb.step();
            const auto saved=gb.save_state();
            Emulator restored(Cartridge(bytes),model);
            restored.load_state(saved);
            check(restored.save_state()==saved,"mid-bootstrap state roundtrip");
            finish(gb); finish(restored); verify(gb,bytes); verify(restored,bytes,2);
            const auto trace=gb.bus().debug_take_io_trace();
            verify_wire(trace); verify_timing(gb,bytes,trace);
            const auto final=gb.save_state(), resumed=restored.save_state();
            check(final==resumed,"mid-bootstrap continuation");
            const auto clocks=gb.cpu().total_cycles();
            gb.reset(); finish(gb); verify(gb,bytes);
            check(gb.cpu().total_cycles()==clocks,"reset timing deterministic");
        }
        std::cout<<"SGB replacement boot contracts passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
