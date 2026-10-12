#pragma once

#include "gameboy/emulator.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace runner_diagnostics {
// Fixed storage, allocated by the runner only when one of these diagnostics is enabled.
template<class T, std::size_t N> struct Ring {
    std::array<T,N> values{};
    std::size_t next{}, count{};
    void push(const T& value) noexcept {
        values[next]=value; next=(next+1)%N; count=std::min(count+1,N);
    }
    template<class F> void each(F f) const {
        for (std::size_t i=0;i<count;++i) f(values[(next+N-count+i)%N]);
    }
};
using Mapping = gameboy::Cartridge::Mmm01Diagnostics;
inline void mapping(std::ostream& out, const Mapping& s, const char* suffix="") {
    const auto field=[&](const char* name, auto value) { out << ' ' << name << suffix << '=' << value; };
    out << std::dec << std::boolalpha;
    field("bank0",s.bank0); field("bank1",s.bank1); field("ram_bank",s.ram_bank);
    field("requested_bank0",s.requested_bank0); field("requested_bank1",s.requested_bank1);
    field("requested_ram_bank",s.requested_ram_bank);
    field("mapped",s.mapped); field("locked",s.mapped); field("mode_locked",s.mode_locked);
    field("ram_enabled",s.ram_enabled); field("ram_present",s.ram_present);
    field("rom_mask",(s.registers[3]>>1)&0x1E); field("ram_mask",(s.registers[0]>>4)&3);
    field("mode",s.registers[3]&1); field("multiplex",(s.registers[3]&0x40)!=0);
    out << " registers" << suffix << '=' << std::hex << std::setfill('0');
    for (auto r:s.registers) out << std::setw(2) << unsigned(r);
    out << std::dec;
}
inline void registers(std::ostream& out, const gameboy::CpuRegisters& r) {
    out << std::hex << std::setfill('0') << " next_pc=" << std::setw(4) << r.pc
        << " sp=" << std::setw(4) << r.sp
        << " af=" << std::setw(4) << ((unsigned(r.a)<<8)|r.f)
        << " bc=" << std::setw(4) << ((unsigned(r.b)<<8)|r.c)
        << " de=" << std::setw(4) << ((unsigned(r.d)<<8)|r.e)
        << " hl=" << std::setw(4) << ((unsigned(r.h)<<8)|r.l) << std::dec;
}
struct Instruction {
    std::uint64_t cycle{};
    unsigned duration{};
    std::uint16_t pc{}, sp_before{};
    std::uint8_t opcode{}, rom_byte{}, raw_bus_byte{}, cpu_bus_byte{};
    bool in_rom{}, boot{}, mmm{};
    gameboy::MemoryBus::OamDmaDiagnostics dma{};
    gameboy::CpuRegisters after{};
    Mapping state{};
};
inline void instruction(std::ostream& out, const char* prefix, const Instruction& i) {
    out << prefix << " cycle=" << i.cycle << " duration=" << i.duration
        << std::hex << std::setfill('0') << " pc=" << std::setw(4) << i.pc
        << " opcode=" << std::setw(2) << unsigned(i.opcode)
        << " rom_byte=" << std::setw(2) << unsigned(i.rom_byte)
        << " raw_bus_byte=" << std::setw(2) << unsigned(i.raw_bus_byte)
        << " cpu_bus_byte=" << std::setw(2) << unsigned(i.cpu_bus_byte)
        << " sp_before=" << std::setw(4) << i.sp_before;
    registers(out,i.after);
    out << std::boolalpha << " in_rom=" << i.in_rom << " boot_enabled=" << i.boot
        << " dma_active=" << i.dma.active << " matches_rom=" << (i.in_rom && i.opcode==i.rom_byte)
        << " provenance_sample=after_opcode_fetch mmm01=" << i.mmm;
    if (i.mmm) {
        mapping(out,i.state);
        if (i.in_rom) {
            const auto bank=i.pc<0x4000?i.state.bank0:i.state.bank1;
            const auto physical=bank*0x4000+(i.pc&0x3FFF);
            out << " physical_offset=" << physical
                << " file_offset=" << (i.state.menu_first?(physical+0x8000)%(i.state.rom_banks*0x4000):physical);
        } else out << " physical_offset=none file_offset=none";
    }
    out << '\n';
}
struct Write {
    std::uint64_t cycle{};
    std::uint16_t address{}, pc{};
    std::uint8_t value{}, lcd_before{}, lcd_after{};
    bool fetched{}, mmm{}, lcd_off{};
    Mapping before{}, after{};
    gameboy::CpuRegisters at_write{};
};
inline void write(std::ostream& out, const char* prefix, const Write& w) {
    out << prefix << " cycle=" << w.cycle << std::hex << std::setfill('0') << " pc=";
    if(w.fetched) out << std::setw(4) << w.pc; else out << "none";
    out << " address=" << std::setw(4) << w.address << " value=" << std::setw(2) << unsigned(w.value);
    registers(out,w.at_write);
    out << " boundary=cpu_write transition=" << (w.fetched?"instruction":"interrupt");
    if (w.mmm) { mapping(out,w.before,"_before"); mapping(out,w.after,"_after"); }
    out << '\n';
}
class Diagnostics {
public:
    Diagnostics(gameboy::Emulator& emulator, const std::filesystem::path& path,
                std::uint64_t limit, std::uint64_t nop_threshold, bool lcd)
        : e(emulator), cap(limit?std::min<std::uint64_t>(limit,4096):4096), threshold(nop_threshold), lcd_enabled(lcd) {
        Mapping state;
        mmm=e.bus().cartridge().debug_mmm01(state);
        if (mmm && !path.empty()) {
            if(path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
            file.open(path,std::ios::trunc);
            if(!file) throw std::runtime_error("could not open MMM01 trace: "+path.string());
            file << "MMM01_STATE trace_version=1 boundary=initial rom_banks=" << state.rom_banks
                 << " layout=" << (state.menu_first?"menu_first_normalized":"as_loaded")
                 << " record_limit=" << cap;
            mapping(file,state); file << '\n';
        }
    }
    void before_step() noexcept {
        fetched=false; pending_count=0;
        step_cycle=e.cpu().total_cycles(); step_bus=e.bus().debug_bus_cycles();
        current={}; current.cycle=step_cycle; current.sp_before=e.cpu().registers().sp;
    }
    void fetch(std::uint16_t pc, std::uint8_t opcode) noexcept {
        fetched=true; current.pc=pc; current.opcode=opcode;
        // These are read-only samples at the real fetch callback, not guessed opcodes.
        current.in_rom=pc<0x8000;
        current.rom_byte=current.in_rom?e.bus().cartridge().read(pc):0;
        current.raw_bus_byte=e.bus().read8(pc); current.cpu_bus_byte=e.bus().debug_cpu_read8(pc);
        current.boot=e.bus().boot_rom_enabled(); current.dma=e.bus().debug_oam_dma();
        current.mmm=e.bus().cartridge().debug_mmm01(current.state);
    }
    static void observe_write(void* context, const gameboy::MemoryBus& bus,
                              std::uint16_t address, std::uint8_t value, bool before) noexcept {
        auto& self=*static_cast<Diagnostics*>(context);
        if (address<0x8000 && !self.mmm) return;
        if (before) {
            self.pending={}; auto& w=self.pending;
            w.address=address; w.value=value; w.pc=self.current.pc; w.fetched=self.fetched;
            w.cycle=self.step_cycle+(bus.debug_bus_cycles()-self.step_bus);
            w.at_write=self.e.cpu().registers();
            w.mmm=bus.cartridge().debug_mmm01(w.before);
            if(address==0xFF40) w.lcd_before=bus.read8(address);
        } else {
            auto& w=self.pending;
            (void)bus.cartridge().debug_mmm01(w.after);
            if(address==0xFF40) {
                w.lcd_after=bus.read8(address);
                w.lcd_off=self.lcd_enabled && (w.lcd_before&0x80) && !(w.lcd_after&0x80);
            }
            // SM83 instructions (including interrupt pushes) perform at most two writes.
            if(self.pending_count<self.pending_writes.size()) self.pending_writes[self.pending_count++]=w;
        }
    }
    void stepped(unsigned duration) {
        current.duration=duration; current.after=e.cpu().registers();
        if (fetched && !first_fetch) {
            first_fetch=true;
            if(file.is_open()) instruction(file,"MMM01_FETCH_FIRST",current);
        }
        for(std::size_t i=0;i<pending_count;++i) {
            const auto& w=pending_writes[i];
            if(w.address<0x8000) {
                writes.push(w);
                if(file.is_open()) {
                    if(written<cap) { write(file,"MMM01_WRITE",w); ++written; }
                    else if(!truncated) { file << "MMM01_TRUNCATED record_limit=" << cap << '\n'; truncated=true; }
                }
            }
        }
        if(threshold && !nop_reported && fetched && current.opcode==0) {
            if(!nop_count) { nop_start=current; pre_nop=history; }
            ++nop_count;
            if(nop_count>=threshold) {
                nop_reported=true;
                std::cerr << "NOP_RUN count=" << nop_count << " start_cycle=" << nop_start.cycle
                          << std::hex << " start_pc=" << nop_start.pc << std::dec << '\n';
                instruction(std::cerr,"NOP_RUN_CURRENT",current);
                instruction(std::cerr,"NOP_RUN_START",nop_start);
                pre_nop.each([](const Instruction& i){instruction(std::cerr,"NOP_RUN_HISTORY",i);});
                dump_writes("NOP_RUN_MMM01_WRITE");
            }
        } else nop_count=0;
        if(fetched) history.push(current);
        for(std::size_t i=0;i<pending_count;++i) {
            const auto& w=pending_writes[i];
            if(w.lcd_off && !lcd_reported) {
                lcd_reported=true;
                std::cerr << "LCD_OFF cycle=" << w.cycle << " step_cycle=" << step_cycle
                          << " after_cycle=" << e.cpu().total_cycles() << std::hex
                          << " lcdc_before=" << unsigned(w.lcd_before) << " lcdc_after=" << unsigned(w.lcd_after)
                          << std::dec << '\n';
                write(std::cerr,"LCD_OFF_WRITE",w);
                if(fetched) instruction(std::cerr,"LCD_OFF_INSTRUCTION",current);
                history.each([](const Instruction& i){instruction(std::cerr,"LCD_OFF_HISTORY",i);});
                dump_writes("LCD_OFF_MMM01_WRITE");
            }
        }
    }
    void finish() {
        if(file.is_open()) { file.flush(); if(!file) throw std::runtime_error("could not write MMM01 trace"); }
    }
private:
    void dump_writes(const char* prefix) const { writes.each([&](const Write& w){write(std::cerr,prefix,w);}); }
    gameboy::Emulator& e;
    std::ofstream file;
    std::uint64_t cap{}, threshold{}, written{}, step_cycle{}, step_bus{}, nop_count{};
    bool lcd_enabled{}, mmm{}, fetched{}, first_fetch{}, truncated{}, nop_reported{}, lcd_reported{};
    Instruction current{}, nop_start{};
    Ring<Instruction,64> history{}, pre_nop{};
    Ring<Write,32> writes{};
    Write pending{};
    std::array<Write,2> pending_writes{};
    std::size_t pending_count{};
};
} // namespace runner_diagnostics
