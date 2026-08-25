#pragma once

#include "gameboy/cpu.hpp"
#include "gameboy/hardware_model.hpp"
#include "gameboy/memory_bus.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace gameboy {

class SaveStateCodec;

class SaveStateError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Emulator {
public:
    explicit Emulator(Cartridge cartridge,
                      HardwareModel model = HardwareModel::automatic);

    static Emulator from_file(
        const std::filesystem::path& path,
        HardwareModel model = HardwareModel::automatic);

    void reset() noexcept;
    [[nodiscard]] unsigned step();

    [[nodiscard]] const Cpu& cpu() const noexcept;
    [[nodiscard]] const MemoryBus& bus() const noexcept;
    [[nodiscard]] MemoryBus& bus() noexcept;
    [[nodiscard]] const Ppu::Framebuffer& framebuffer() const noexcept;
    [[nodiscard]] bool frame_ready() const noexcept;
    void consume_frame() noexcept;
    [[nodiscard]] std::vector<std::int16_t> take_audio_samples();
    void set_button(Button button, bool pressed) noexcept;
    void flush_battery();
    [[nodiscard]] bool has_battery() const noexcept;
    [[nodiscard]] bool has_rtc() const noexcept;
    [[nodiscard]] bool has_rumble() const noexcept;
    [[nodiscard]] bool rumble_active() const noexcept;
    [[nodiscard]] bool has_camera() const noexcept;
    void set_camera_frame(const std::uint8_t* grayscale,
                          std::size_t size) noexcept;
    [[nodiscard]] std::vector<std::uint8_t> export_battery_ram() const;
    void import_battery_ram(const std::vector<std::uint8_t>& data);
    [[nodiscard]] std::vector<std::uint8_t> export_battery_save() const;
    void import_battery_save(const std::vector<std::uint8_t>& data);
    [[nodiscard]] std::vector<std::uint8_t> export_rtc_data() const;
    void import_rtc_data(const std::vector<std::uint8_t>& data);
    [[nodiscard]] std::uint64_t rom_fingerprint() const noexcept;
    void set_dmg_compatibility_colors(bool enabled) noexcept;
    [[nodiscard]] std::vector<std::uint8_t> save_state() const;
    void load_state(const std::vector<std::uint8_t>& state);

private:
    friend class SaveStateCodec;

    MemoryBus bus_;
    Cpu cpu_;
    HardwareModel hardware_model_{HardwareModel::dmg};
    DmgPalette automatic_dmg_palette_{grayscale_dmg_palette};
};

} // namespace gameboy
