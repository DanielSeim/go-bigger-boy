// SPDX-License-Identifier: GPL-3.0-or-later
// ROM-free bootstrap contracts. No Nintendo artwork or instructions.
#include "gameboy/emulator.hpp"
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
    for (unsigned i=0x104; i<0x150; ++i) bytes[i]=static_cast<std::uint8_t>(pattern*(i-0x104));
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
            if(low) check(event.cycle-low_clock>=20,"bootstrap low pulse shorter than 5 M-cycles");
            low=false; high_clock=event.cycle;
        } else {
            check(!low && event.cycle-high_clock>=60,"bootstrap high space shorter than 15 M-cycles");
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
        for(auto model:{HardwareModel::sgb,HardwareModel::sgb2})
        for(auto mode:{BootRomMode::replacement_sgb,BootRomMode::animated_dmg})
        for(unsigned pattern:{0U,1U,255U}) {
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
            verify_wire(gb.bus().debug_take_io_trace());
            const auto final=gb.save_state(), resumed=restored.save_state();
            check(final==resumed,"mid-bootstrap continuation");
            const auto clocks=gb.cpu().total_cycles();
            gb.reset(); finish(gb); verify(gb,bytes);
            check(gb.cpu().total_cycles()==clocks,"reset timing deterministic");
        }
        std::cout<<"SGB replacement boot contracts passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
