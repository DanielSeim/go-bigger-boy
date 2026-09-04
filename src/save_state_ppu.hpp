#pragma once

#include "save_state_format.hpp"

namespace gameboy {

class Ppu;

class SaveStatePpuCodec final {
public:
    static void write(save_state_format::Writer& writer, const Ppu& ppu);
    static void read(save_state_format::Reader& reader, Ppu& ppu);
};

} // namespace gameboy
