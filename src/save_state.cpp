#include "gameboy/emulator.hpp"
#include "save_state_bus.hpp"
#include "save_state_container.hpp"
#include "save_state_cpu.hpp"
#include "save_state_format.hpp"

#include <string>

namespace gameboy {
namespace {

using save_state_format::Reader;
using save_state_format::Writer;
} // namespace

class SaveStateCodec {
public:
    [[nodiscard]] static std::vector<std::uint8_t> encode(
        const Emulator& emulator) {
        emulator.bus_.cartridge_.update_rtc();

        Writer payload;
        write_cpu(payload, emulator.cpu_);
        write_bus(payload, emulator.bus_);
        return save_state_container::encode(emulator.rom_fingerprint(),
                                            payload.data());
    }

    static void decode(Emulator& emulator,
                       const std::vector<std::uint8_t>& state) {
        const auto decoded = save_state_container::decode(
            state, emulator.rom_fingerprint());
        Reader payload(decoded.payload);
        read_cpu(payload, emulator.cpu_);
        read_bus(payload, emulator.bus_, decoded.version);
        payload.finish();
    }

private:
    static void write_cpu(Writer& writer, const Cpu& cpu) {
        SaveStateCpuCodec::write(writer, cpu);
    }

    static void read_cpu(Reader& reader, Cpu& cpu) {
        SaveStateCpuCodec::read(reader, cpu);
    }

    static void write_bus(Writer& writer, const MemoryBus& bus) {
        SaveStateBusCodec::write(writer, bus);
    }

    static void read_bus(Reader& reader, MemoryBus& bus,
                         const std::uint32_t version) {
        SaveStateBusCodec::read(reader, bus, version);
    }

};

std::uint64_t Emulator::rom_fingerprint() const noexcept {
    return bus_.cartridge().rom_fingerprint();
}

std::uint64_t Emulator::link_compatibility_id() const noexcept {
    return bus_.cartridge().link_compatibility_id();
}

std::vector<std::uint8_t> Emulator::save_state() const {
    return SaveStateCodec::encode(*this);
}

void Emulator::load_state(const std::vector<std::uint8_t>& state) {
    const auto backup = save_state();
    try {
        SaveStateCodec::decode(*this, state);
    } catch (...) {
        try {
            SaveStateCodec::decode(*this, backup);
        } catch (...) {
            // The in-memory backup was produced by this exact codec and ROM.
        }
        throw;
    }
}

} // namespace gameboy
