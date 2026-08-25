#include "gameboy/emulator.hpp"

#include <utility>

namespace gameboy {

Emulator::Emulator(Cartridge cartridge, const HardwareModel model)
    : bus_(std::move(cartridge)) {
    hardware_model_ = model == HardwareModel::automatic
                          ? (bus_.cgb_mode() ? HardwareModel::cgb
                                             : HardwareModel::dmg)
                          : model;
    automatic_dmg_palette_ = cgb_compatibility_palette(
        bus_.cartridge().cgb_compatibility_palette_id());
    cpu_.reset(hardware_model_);
    bus_.initialize_post_boot(hardware_model_);
}

Emulator Emulator::from_file(const std::filesystem::path& path,
                             const HardwareModel model) {
    return Emulator(Cartridge::from_file(path), model);
}

void Emulator::reset() noexcept {
    cpu_.reset(hardware_model_);
}

unsigned Emulator::step() {
    return cpu_.step(bus_);
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

bool Emulator::has_battery() const noexcept {
    return bus_.cartridge().has_battery();
}

bool Emulator::has_rtc() const noexcept { return bus_.cartridge().has_rtc(); }

bool Emulator::has_rumble() const noexcept {
    return bus_.cartridge().has_rumble();
}

bool Emulator::rumble_active() const noexcept {
    return bus_.cartridge().rumble_active();
}

bool Emulator::has_camera() const noexcept {
    return bus_.cartridge().has_camera();
}

void Emulator::set_camera_frame(const std::uint8_t* grayscale,
                                const std::size_t size) noexcept {
    bus_.cartridge().set_camera_frame(grayscale, size);
}

std::vector<std::uint8_t> Emulator::export_battery_ram() const {
    return bus_.cartridge().export_battery_ram();
}

void Emulator::import_battery_ram(const std::vector<std::uint8_t>& data) {
    bus_.cartridge().import_battery_ram(data);
}

std::vector<std::uint8_t> Emulator::export_battery_save() const {
    return bus_.cartridge().export_battery_save();
}

void Emulator::import_battery_save(const std::vector<std::uint8_t>& data) {
    bus_.cartridge().import_battery_save(data);
}

std::vector<std::uint8_t> Emulator::export_rtc_data() const {
    return bus_.cartridge().export_rtc_data();
}

void Emulator::import_rtc_data(const std::vector<std::uint8_t>& data) {
    bus_.cartridge().import_rtc_data(data);
}

void Emulator::set_dmg_compatibility_colors(const bool enabled) noexcept {
    bus_.set_dmg_palette(enabled ? automatic_dmg_palette_
                                 : grayscale_dmg_palette);
}

} // namespace gameboy
