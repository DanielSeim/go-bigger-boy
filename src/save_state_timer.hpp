#pragma once

#include "save_state_format.hpp"

namespace gameboy {

class Timer;

class SaveStateTimerCodec final {
public:
    static void write(save_state_format::Writer& writer, const Timer& timer);
    static void read(save_state_format::Reader& reader, Timer& timer);
};

} // namespace gameboy
