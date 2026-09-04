#include "save_state_joypad.hpp"

#include "gameboy/joypad.hpp"

namespace gameboy {

void SaveStateJoypadCodec::write(save_state_format::Writer& writer,
                                 const Joypad& joypad) {
    writer.u8(joypad.select_);
    writer.u8(joypad.directions_);
    writer.u8(joypad.actions_);
}

void SaveStateJoypadCodec::read(save_state_format::Reader& reader,
                                Joypad& joypad) {
    joypad.select_ = static_cast<std::uint8_t>(reader.u8() & 0x30);
    joypad.directions_ = static_cast<std::uint8_t>(reader.u8() & 0x0F);
    joypad.actions_ = static_cast<std::uint8_t>(reader.u8() & 0x0F);
}

} // namespace gameboy
