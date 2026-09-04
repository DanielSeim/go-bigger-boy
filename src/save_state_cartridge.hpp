#pragma once

#include "save_state_format.hpp"

namespace gameboy {

class Cartridge;

class SaveStateCartridgeCodec final {
public:
    static void write(save_state_format::Writer& writer,
                      const Cartridge& cartridge);
    static void read(save_state_format::Reader& reader,
                     Cartridge& cartridge);
};

} // namespace gameboy
