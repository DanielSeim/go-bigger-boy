#pragma once

#include "save_state_format.hpp"

namespace gameboy {

class Apu;

class SaveStateApuCodec final {
public:
    static void write(save_state_format::Writer& writer, const Apu& apu);
    static void read(save_state_format::Reader& reader, Apu& apu,
                     std::uint32_t version);
};

} // namespace gameboy
