#include "gameboy/emulator.hpp"
#include "gameboy/boot_splash.hpp"
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
        // Append the phase rather than changing legacy CPU/bus field offsets.
        SaveStateCpuCodec::write_irq_phase(payload, emulator.cpu_);
        payload.u8(static_cast<std::uint8_t>((emulator.splash_enabled_ ? 1 : 0) |
                                            (emulator.splash_skipped_ ? 2 : 0)));
        payload.u64(emulator.splash_consumed_frame_);
        payload.u64(emulator.splash_handoff_cycles_);
        // Queued PCM is deliberately not serialized by the APU codec. A
        // restored presentation must start at the saved clock, not replay
        // the interval since the source last drained its pending queue.
        payload.u64(emulator.splash_enabled_
            ? boot_splash_sample_time(emulator.cpu_.total_cycles()) : 0);
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
        if (decoded.version >= 41)
            SaveStateCpuCodec::read_irq_phase(payload, emulator.cpu_);
        emulator.splash_enabled_ = emulator.splash_skipped_ = false;
        emulator.splash_consumed_frame_ = emulator.splash_handoff_cycles_ = emulator.splash_audio_cursor_ = 0;
        emulator.splash_cached_frame_ = UINT64_MAX;
        if (decoded.version >= 42) {
            const auto flags = payload.u8();
            emulator.splash_enabled_ = (flags & 1) != 0;
            emulator.splash_skipped_ = (flags & 2) != 0;
            emulator.splash_consumed_frame_ = payload.u64();
            emulator.splash_handoff_cycles_ = payload.u64();
            emulator.splash_audio_cursor_ = payload.u64();
            const auto cycles = emulator.cpu_.total_cycles();
            if ((flags & ~3U) || (emulator.splash_skipped_ && !emulator.splash_enabled_) ||
                emulator.splash_consumed_frame_ > cycles / boot_splash_frame_cycles ||
                emulator.splash_handoff_cycles_ > cycles ||
                emulator.splash_audio_cursor_ > boot_splash_sample_time(cycles) ||
                (emulator.bus_.boot_rom_enabled() && emulator.splash_handoff_cycles_ != 0) ||
                (!emulator.splash_enabled_ && (emulator.splash_consumed_frame_ ||
                    emulator.splash_handoff_cycles_ || emulator.splash_audio_cursor_)))
                throw SaveStateError("Invalid startup presentation state");
        }
        payload.finish();
        if (emulator.splash_enabled_) {
            if (!emulator.splash_pixels_)
                emulator.splash_pixels_ = std::make_unique<Ppu::Framebuffer>();
            prepare_boot_splash_audio(emulator.hardware_model_);
        }
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

LinkCompatibilityProfile Emulator::link_compatibility_profile() const noexcept {
    return bus_.cartridge().link_compatibility_profile();
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
