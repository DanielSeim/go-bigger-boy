#include "save_state_timer.hpp"

#include "gameboy/timer.hpp"

namespace gameboy {

void SaveStateTimerCodec::write(save_state_format::Writer& writer,
                                const Timer& timer) {
    writer.u16(timer.divider_counter_);
    writer.u8(timer.counter_);
    writer.u8(timer.modulo_);
    writer.u8(timer.control_);
    writer.u32(timer.reload_delay_);
    writer.u32(timer.apu_ticks_);
}

void SaveStateTimerCodec::read(save_state_format::Reader& reader,
                               Timer& timer) {
    timer.divider_counter_ = reader.u16();
    timer.counter_ = reader.u8();
    timer.modulo_ = reader.u8();
    timer.control_ = static_cast<std::uint8_t>(reader.u8() & 0x07);
    timer.reload_delay_ = reader.u32();
    timer.apu_ticks_ = reader.u32();
}

} // namespace gameboy
