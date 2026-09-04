#pragma once

#include "save_state_format.hpp"

namespace gameboy {

class Cpu;

// Private save-state boundary for CPU fields. Keeping this separate from the
// bus codec makes CPU-state migrations reviewable without changing the public
// emulator API or the on-disk field order.
class SaveStateCpuCodec final {
public:
    static void write(save_state_format::Writer& writer, const Cpu& cpu);
    static void read(save_state_format::Reader& reader, Cpu& cpu);
};

} // namespace gameboy
