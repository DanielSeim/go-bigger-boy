#include "gameboy/emulator.hpp"

#include <utility>

namespace gameboy {

Emulator::Emulator(Cartridge cartridge) : bus_(std::move(cartridge)) {
    bus_.initialize_post_boot();
}

Emulator Emulator::from_file(const std::filesystem::path& path) {
    return Emulator(Cartridge::from_file(path));
}

void Emulator::reset() noexcept {
    cpu_.reset();
}

unsigned Emulator::step() {
    const auto cycles = cpu_.step(bus_);
    if (!cpu_.stopped()) {
        bus_.tick(cycles);
    }
    return cycles;
}

const Cpu& Emulator::cpu() const noexcept {
    return cpu_;
}

const MemoryBus& Emulator::bus() const noexcept {
    return bus_;
}

MemoryBus& Emulator::bus() noexcept {
    return bus_;
}

const Ppu::Framebuffer& Emulator::framebuffer() const noexcept {
    return bus_.framebuffer();
}

bool Emulator::frame_ready() const noexcept { return bus_.frame_ready(); }

void Emulator::consume_frame() noexcept { bus_.consume_frame(); }

std::vector<std::int16_t> Emulator::take_audio_samples() {
    return bus_.take_audio_samples();
}

void Emulator::set_button(const Button button, const bool pressed) noexcept {
    bus_.set_button(button, pressed);
}

void Emulator::flush_battery() { bus_.flush_battery(); }

} // namespace gameboy
