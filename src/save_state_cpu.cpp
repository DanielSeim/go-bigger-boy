#include "save_state_cpu.hpp"

#include "gameboy/cpu.hpp"

namespace gameboy {

void SaveStateCpuCodec::write(save_state_format::Writer& writer,
                              const Cpu& cpu) {
    writer.u8(cpu.registers_.a);
    writer.u8(cpu.registers_.f);
    writer.u8(cpu.registers_.b);
    writer.u8(cpu.registers_.c);
    writer.u8(cpu.registers_.d);
    writer.u8(cpu.registers_.e);
    writer.u8(cpu.registers_.h);
    writer.u8(cpu.registers_.l);
    writer.u16(cpu.registers_.sp);
    writer.u16(cpu.registers_.pc);
    writer.boolean(cpu.ime_);
    writer.boolean(cpu.halted_);
    writer.boolean(cpu.stopped_);
    writer.boolean(cpu.halt_bug_);
    writer.u32(cpu.ime_enable_delay_);
    writer.u32(cpu.step_cycles_);
    writer.u64(cpu.total_cycles_);
}

void SaveStateCpuCodec::read(save_state_format::Reader& reader, Cpu& cpu) {
    cpu.registers_.a = reader.u8();
    cpu.registers_.f = static_cast<std::uint8_t>(reader.u8() & 0xF0);
    cpu.registers_.b = reader.u8();
    cpu.registers_.c = reader.u8();
    cpu.registers_.d = reader.u8();
    cpu.registers_.e = reader.u8();
    cpu.registers_.h = reader.u8();
    cpu.registers_.l = reader.u8();
    cpu.registers_.sp = reader.u16();
    cpu.registers_.pc = reader.u16();
    cpu.ime_ = reader.boolean();
    cpu.halted_ = reader.boolean();
    cpu.stopped_ = reader.boolean();
    cpu.halt_bug_ = reader.boolean();
    cpu.ime_enable_delay_ = reader.u32();
    cpu.step_cycles_ = reader.u32();
    cpu.total_cycles_ = reader.u64();
}

} // namespace gameboy
