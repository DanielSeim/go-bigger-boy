#pragma once

#include "save_state_format.hpp"

namespace gameboy {

class Joypad;

class SaveStateJoypadCodec final {
public:
    static void write(save_state_format::Writer& writer, const Joypad& joypad);
    static void read(save_state_format::Reader& reader, Joypad& joypad);
};

} // namespace gameboy
