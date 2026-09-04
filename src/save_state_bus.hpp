#pragma once

#include "save_state_format.hpp"

namespace gameboy {

class MemoryBus;

// Owns the serialized MemoryBus payload and its versioned compatibility rules.
// Keeping this boundary private prevents frontends from depending on wire
// details while allowing the emulator wrapper to remain a small coordinator.
class SaveStateBusCodec final {
public:
    static void write(save_state_format::Writer& writer,
                      const MemoryBus& bus);
    static void read(save_state_format::Reader& reader, MemoryBus& bus,
                     std::uint32_t version);
};

} // namespace gameboy
