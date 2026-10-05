#include "gameboy/emulator.hpp"
#include "gameboy/boot_splash.hpp"

#include <stdexcept>
#include <utility>

namespace gameboy {

Emulator::Emulator(Cartridge cartridge, const HardwareModel model,
                   const BootRomMode boot_rom_mode)
    : bus_(std::move(cartridge)), boot_rom_mode_(boot_rom_mode) {
    hardware_model_ = resolve_hardware_model(model, bus_.cgb_mode(),
                                             bus_.cartridge().supports_sgb());
    automatic_dmg_palette_ = cgb_compatibility_palette(
        bus_.cartridge().cgb_compatibility_palette_id());
    splash_enabled_ = boot_rom_mode_ == BootRomMode::animated_dmg;
    if (boot_rom_mode_ == BootRomMode::replacement_dmg || splash_enabled_) {
        if (hardware_model_ != HardwareModel::dmg && hardware_model_ != HardwareModel::mgb) {
            throw std::invalid_argument("Monochrome replacement boot requires DMG or MGB hardware");
        }
        bus_.initialize_dmg_power_on();
        bus_.install_boot_rom(hardware_model_ == HardwareModel::mgb
            ? (splash_enabled_ ? mgb_animated_boot_rom() : mgb_boot_rom())
            : (splash_enabled_ ? dmg_animated_boot_rom() : dmg_boot_rom()));
        cpu_.reset_boot();
        // Precompute outside the playback loop, not at the first audible note.
        if (splash_enabled_) {
            splash_pixels_ = std::make_unique<Ppu::Framebuffer>();
            prepare_boot_splash_audio();
        }
        return;
    }
    bus_.initialize_post_boot(hardware_model_);
    if (boot_rom_mode_ == BootRomMode::diagnostic) {
        bus_.install_boot_rom(diagnostic_boot_rom(hardware_model_));
        cpu_.reset_boot();
    } else {
        cpu_.reset(hardware_model_);
    }
}

Emulator Emulator::from_file(const std::filesystem::path& path,
                             const HardwareModel model,
                             const BootRomMode boot_rom_mode) {
    return Emulator(Cartridge::from_file(path), model, boot_rom_mode);
}

void Emulator::reset() noexcept {
    splash_enabled_ = boot_rom_mode_ == BootRomMode::animated_dmg;
    splash_skipped_ = false;
    splash_consumed_frame_ = splash_handoff_cycles_ = splash_audio_cursor_ = 0;
    splash_cached_frame_ = UINT64_MAX;
    if (boot_rom_mode_ == BootRomMode::replacement_dmg || splash_enabled_) {
        bus_.initialize_dmg_power_on();
        bus_.install_boot_rom(hardware_model_ == HardwareModel::mgb
            ? (splash_enabled_ ? mgb_animated_boot_rom() : mgb_boot_rom())
            : (splash_enabled_ ? dmg_animated_boot_rom() : dmg_boot_rom()));
        cpu_.reset_boot();
    } else if (boot_rom_mode_ == BootRomMode::diagnostic) {
        bus_.initialize_post_boot(hardware_model_);
        bus_.install_boot_rom(diagnostic_boot_rom(hardware_model_));
        cpu_.reset_boot();
    } else {
        // A version-40 state may have restored an in-progress firmware boot
        // into a post-boot emulator. Reset follows the configured mode: do
        // not leave the lower cartridge vectors covered by that image.
        if (bus_.boot_rom_enabled()) {
            bus_.write8(0xFF50, 1);
            bus_.initialize_post_boot(hardware_model_);
        }
        cpu_.reset(hardware_model_);
    }
}

unsigned Emulator::step() {
    const bool was_booting = splash_enabled_ && bus_.boot_rom_enabled();
    const auto cycles = cpu_.step(bus_);
    if (was_booting && !bus_.boot_rom_enabled()) splash_handoff_cycles_ = cpu_.total_cycles();
    return cycles;
}

const Cpu& Emulator::cpu() const noexcept {
    return cpu_;
}

void Emulator::set_cpu_registers(CpuRegisters registers) noexcept {
    cpu_.load_registers(registers);
}

const MemoryBus& Emulator::bus() const noexcept {
    return bus_;
}

MemoryBus& Emulator::bus() noexcept {
    return bus_;
}

const Ppu::Framebuffer& Emulator::framebuffer() const noexcept {
    if (startup_animation_active()) {
        const auto frame = cpu_.total_cycles() / boot_splash_frame_cycles;
        if (frame != splash_cached_frame_) {
            render_boot_splash(*splash_pixels_, frame);
            splash_cached_frame_ = frame;
        }
        return *splash_pixels_;
    }
    return bus_.framebuffer();
}

const Ppu::SgbFramebuffer& Emulator::sgb_framebuffer() const noexcept {
    return bus_.sgb_framebuffer();
}

bool Emulator::startup_animation_active() const noexcept {
    return splash_enabled_ && !splash_skipped_ && bus_.boot_rom_enabled();
}

bool Emulator::frame_ready() const noexcept {
    return bus_.frame_ready() || (startup_animation_active() &&
        cpu_.total_cycles() / boot_splash_frame_cycles > splash_consumed_frame_);
}

void Emulator::consume_frame() noexcept {
    if (splash_enabled_) splash_consumed_frame_ = cpu_.total_cycles() / boot_splash_frame_cycles;
    bus_.consume_frame();
}

std::vector<std::int16_t> Emulator::take_audio_samples() {
    auto samples = bus_.take_audio_samples();
    if (splash_enabled_) {
        const auto cutoff = splash_handoff_cycles_ ? boot_splash_sample_time(splash_handoff_cycles_)
            : bus_.boot_rom_enabled() ? UINT64_MAX : 0;
        for (std::size_t index = 0; index < samples.size() / 2; ++index) {
            const auto time = splash_audio_cursor_ + index;
            if (time >= cutoff) break;
            const auto value = splash_skipped_ ? 0 : boot_splash_sample(time);
            samples[index * 2] = samples[index * 2 + 1] = value;
        }
        // APU buffers are bounded. Advance to now even if undrained samples
        // were dropped; never replay an old note after a long pause.
        splash_audio_cursor_ = boot_splash_sample_time(cpu_.total_cycles());
    }
    return samples;
}

void Emulator::set_audio_enabled(const bool enabled) noexcept {
    if (splash_enabled_ && enabled != bus_.audio_enabled())
        splash_audio_cursor_ = boot_splash_sample_time(cpu_.total_cycles());
    bus_.set_audio_enabled(enabled);
}

bool Emulator::audio_enabled() const noexcept { return bus_.audio_enabled(); }

void Emulator::set_button(const Button button, const bool pressed) noexcept {
    if (startup_animation_active() && pressed && (button == Button::a || button == Button::start)) {
        splash_skipped_ = true;
        return;
    }
    bus_.set_button(button, pressed);
}

void Emulator::set_player_button(const std::uint8_t player,
                                  const Button button,
                                  const bool pressed) noexcept {
    if (player == 0) {
        set_button(button, pressed);
        return;
    }
    bus_.set_player_button(player, button, pressed);
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
