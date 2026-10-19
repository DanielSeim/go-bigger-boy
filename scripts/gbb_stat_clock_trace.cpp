// SPDX-License-Identifier: GPL-3.0-or-later
// Optional authored-ROM clock witness; uses existing read-only debug observers.
#include "gameboy/emulator.hpp"
#include "stat_clock_trace.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

struct Context { StatClockTrace trace{}; gameboy::Emulator* emulator{}; };
static void instruction(void* opaque, std::uint16_t pc, std::uint8_t) noexcept {
    auto& c = *static_cast<Context*>(opaque);
    const auto& b = c.emulator->bus();
    stat_clock_observe(&c.trace,b.debug_normalized_clocks(),b.read8(0xff0f));
    stat_clock_vector(&c.trace,b.debug_normalized_clocks(),pc);
}
static void interrupt(void* opaque, std::uint16_t pc, std::uint8_t pending, bool after_fetch) noexcept {
    auto& c = *static_cast<Context*>(opaque);
    const auto& b = c.emulator->bus();
    stat_clock_observe(&c.trace,b.debug_normalized_clocks(),b.read8(0xff0f));
    stat_clock_sample(&c.trace,b.debug_normalized_clocks(),pc,pending,after_fetch);
}
static void write(void* opaque, const gameboy::MemoryBus& b, std::uint16_t address,
                  std::uint8_t value, bool before) noexcept {
    auto& c = *static_cast<Context*>(opaque);
    if (address == 0xff40 && before && (value & 0x80) && !(b.read8(address) & 0x80))
        stat_clock_lcd(&c.trace,b.debug_normalized_clocks());
    stat_clock_observe(&c.trace,b.debug_normalized_clocks(),b.read8(0xff0f));
}
int main(int argc, char** argv) {
    if (argc < 3 || argc > 4 || (argc == 4 && std::string(argv[3]) != "--no-observers")) return 2;
    try {
        std::ifstream input(argv[1],std::ios::binary), boot_file(argv[2],std::ios::binary);
        if (!input || !boot_file) return 2;
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input},{}};
        std::vector<std::uint8_t> boot{std::istreambuf_iterator<char>{boot_file},{}};
        if (bytes.size() != 32768 || boot.size() != 256) return 2;
        gameboy::DiagnosticBootRom image{};
        std::copy(boot.begin(),boot.end(),image.begin());
        gameboy::Emulator e{gameboy::Cartridge{std::move(bytes)},gameboy::HardwareModel::cgb_e,
                           gameboy::BootRomMode::replacement};
        e.bus().install_boot_rom(image);
        Context c{}; c.emulator = &e;
        const bool enabled = argc == 3;
        if (enabled) {
            e.debug_set_cpu_instruction_observer(instruction,&c);
            e.debug_set_cpu_interrupt_observer(interrupt,&c);
            e.bus().debug_set_control_write_observer(write,&c);
        }
        bool complete = false;
        unsigned zero_steps = 0;
        while (e.bus().debug_normalized_clocks() < STAT_CLOCK_LIMIT && !c.trace.overflow) {
            const auto r = e.cpu().registers();
            const auto begin = e.bus().debug_normalized_clocks();
            if (!c.trace.active && r.pc == 0x1000) {
                c.trace.active = 1; c.trace.origin = begin;
            }
            if (c.trace.active && r.pc == 0x1100) { complete = true; break; }
            const unsigned flags = e.bus().read8(0xff0f) & 31;
            stat_clock_observe(&c.trace,begin,flags);
            (void)e.step();
            const auto after = e.cpu().registers();
            const auto end = e.bus().debug_normalized_clocks();
            if (end == begin && ++zero_steps > 1024) break;
            if (end > begin) zero_steps = 0;
            const unsigned next_flags = e.bus().read8(0xff0f) & 31;
            stat_clock_observe(&c.trace,end,next_flags);
            stat_clock_step(&c.trace,{begin,end,r.pc,after.pc,
                static_cast<std::uint16_t>((r.b<<8)|r.c),
                static_cast<std::uint16_t>((after.b<<8)|after.c),flags,next_flags});
            if (e.frame_ready()) { e.consume_frame(); (void)e.take_audio_samples(); }
        }
        const auto r = e.cpu().registers();
        const std::uint16_t regs[]{r.pc,r.sp,static_cast<std::uint16_t>((r.a<<8)|r.f),
            static_cast<std::uint16_t>((r.b<<8)|r.c),static_cast<std::uint16_t>((r.d<<8)|r.e),
            static_cast<std::uint16_t>((r.h<<8)|r.l)};
        std::uint8_t wram[64];
        for (unsigned i=0;i<64;++i) wram[i]=e.bus().read8(static_cast<std::uint16_t>(0xc000+i));
        const auto state = e.save_state();
        stat_clock_print(&c.trace,"gbb","after_opcode_fetch_cycle",enabled,complete,
                         e.bus().debug_normalized_clocks(),regs,wram,state.size(),stat_clock_hash(state.data(),state.size()),0);
        return complete && !c.trace.overflow ? 0 : 3;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"STAT clock trace failed: %s\n",error.what()); return 3;
    }
}
