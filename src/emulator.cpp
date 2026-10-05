#include "gameboy/emulator.hpp"
#include "gameboy/boot_splash.hpp"

#include <stdexcept>
#include <utility>

namespace gameboy {
namespace {
void reset_post_boot_cpu(Cpu& cpu, MemoryBus& bus, HardwareModel model) noexcept {
    cpu.reset(model);
    if (!is_agb_hardware(model)) return;
    std::uint8_t title = 0;
    if (!bus.cgb_mode()) {
        const auto license = bus.read8(0x14b);
        if (license == 1 || (license == 0x33 && bus.read8(0x144) == '0' && bus.read8(0x145) == '1'))
            for (unsigned address = 0x134; address <= 0x143; ++address)
                title = static_cast<std::uint8_t>(title + bus.read8(static_cast<std::uint16_t>(address)));
    }
    const auto b = static_cast<std::uint8_t>(title + 1);
    const auto flags = static_cast<std::uint8_t>((b == 0 ? 0x80 : 0) | ((title & 15) == 15 ? 0x20 : 0));
    const bool legacy = title == 0x43 || title == 0x58;
    cpu.load_registers({0x11, flags, b, 0, static_cast<std::uint8_t>(bus.cgb_mode() ? 0xff : 0),
        static_cast<std::uint8_t>(bus.cgb_mode() ? 0x56 : 8), static_cast<std::uint8_t>(legacy ? 0x99 : 0),
        static_cast<std::uint8_t>(bus.cgb_mode() ? 0x0d : legacy ? 0x1a : 0x7c), 0xfffe, 0x100});
}
const DiagnosticBootRom& replacement_image(HardwareModel model, bool animated) noexcept {
    if (model == HardwareModel::agb0) return animated ? agb0_animated_boot_rom() : agb0_boot_rom();
    if (model == HardwareModel::agb) return animated ? agb_animated_boot_rom() : agb_boot_rom();
    if (model == HardwareModel::cgb0) return animated ? cgb0_animated_boot_rom() : cgb0_boot_rom();
    if (is_cgb_hardware(model)) return animated ? cgb_animated_boot_rom() : cgb_boot_rom();
    if (model == HardwareModel::dmg0) return animated ? dmg0_animated_boot_rom() : dmg0_boot_rom();
    if (model == HardwareModel::mgb) return animated ? mgb_animated_boot_rom() : mgb_boot_rom();
    return animated ? dmg_animated_boot_rom() : dmg_boot_rom();
}
}

Emulator::Emulator(Cartridge cartridge, const HardwareModel model,
                   const BootRomMode boot_rom_mode)
    : bus_(std::move(cartridge)), boot_rom_mode_(boot_rom_mode) {
    hardware_model_ = resolve_hardware_model(model, bus_.cgb_mode(),
                                             bus_.cartridge().supports_sgb());
    automatic_dmg_palette_ = cgb_compatibility_palette(
        bus_.cartridge().cgb_compatibility_palette_id());
    splash_enabled_ = boot_rom_mode_ == BootRomMode::animated_dmg;
    if (boot_rom_mode_ == BootRomMode::replacement_dmg || splash_enabled_) {
        if (hardware_model_ != HardwareModel::dmg0 && hardware_model_ != HardwareModel::dmg && hardware_model_ != HardwareModel::mgb && !is_cgb_hardware(hardware_model_)) {
            throw std::invalid_argument("Replacement boot requires DMG0, DMG, MGB or CGB hardware");
        }
        if (is_cgb_hardware(hardware_model_)) bus_.initialize_cgb_power_on(hardware_model_);
        else bus_.initialize_dmg_power_on();
        bus_.install_boot_rom(replacement_image(hardware_model_, splash_enabled_));
        cpu_.reset_boot();
        // Precompute outside the playback loop, not at the first audible note.
        if (splash_enabled_) {
            splash_pixels_ = std::make_unique<Ppu::Framebuffer>();
            prepare_boot_splash_audio(hardware_model_);
        }
        return;
    }
    bus_.initialize_post_boot(hardware_model_);
    if (boot_rom_mode_ == BootRomMode::diagnostic) {
        bus_.install_boot_rom(diagnostic_boot_rom(hardware_model_));
        cpu_.reset_boot();
    } else {
        reset_post_boot_cpu(cpu_, bus_, hardware_model_);
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
        if (is_cgb_hardware(hardware_model_)) bus_.initialize_cgb_power_on(hardware_model_);
        else bus_.initialize_dmg_power_on();
        bus_.install_boot_rom(replacement_image(hardware_model_, splash_enabled_));
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
        reset_post_boot_cpu(cpu_, bus_, hardware_model_);
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
            render_boot_splash(*splash_pixels_, frame, hardware_model_);
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
    return splash_enabled_ && !splash_skipped_ && bus_.boot_rom_enabled() &&
        !(hardware_model_ == HardwareModel::dmg0 && bus_.read8(0xff40) == 0x81);
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
            const auto failed = hardware_model_ == HardwareModel::dmg0 && bus_.read8(0xff40) == 0x81;
            const auto value = splash_skipped_ || failed ? 0 : boot_splash_sample(time, hardware_model_);
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
