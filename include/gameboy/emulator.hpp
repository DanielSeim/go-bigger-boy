#pragma once

#include "gameboy/cpu.hpp"
#include "gameboy/memory_bus.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace gameboy {

class Emulator {
public:
    explicit Emulator(Cartridge cartridge);

    static Emulator from_file(const std::filesystem::path& path);

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

private:
    MemoryBus bus_;
    Cpu cpu_;
};

} // namespace gameboy
