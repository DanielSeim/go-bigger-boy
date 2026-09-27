#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace gameboy {

// Minimal SNES-side building blocks for a clean-room SGB sound host. These
// expose memory and communication, not a running 65C816, SPC700, or DSP.
class SgbProgramRom final {
public:
    enum class Error : std::uint8_t {
        none,
        unsupported_size,
        unsupported_mapping,
        invalid_reset_vector,
    };

    [[nodiscard]] static Error validate(const std::vector<std::uint8_t>& bytes) noexcept;
    [[nodiscard]] static SgbProgramRom from_file(const std::filesystem::path& path);

    explicit SgbProgramRom(std::vector<std::uint8_t> bytes);
    [[nodiscard]] std::uint8_t read(std::uint8_t bank,
                                    std::uint16_t address) const noexcept;
    [[nodiscard]] std::uint16_t reset_vector() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

private:
    std::vector<std::uint8_t> bytes_;
};

// Models the S-SMP's addressable RAM, host ports, IPL overlay, timers, and DSP
// register access. A caller must supply its own legally obtained 64-byte IPL
// image before attempting to run an SPC700 CPU. DSP synthesis is deliberately
// absent until its behavior has independent tests.
class SnesApuBus final {
public:
    using IplRom = std::array<std::uint8_t, 64>;

    void reset() noexcept;
    void install_ipl(const IplRom& image) noexcept;
    [[nodiscard]] bool has_ipl() const noexcept { return has_ipl_; }

    [[nodiscard]] std::uint8_t spc_read(std::uint16_t address) noexcept;
    void spc_write(std::uint16_t address, std::uint8_t value) noexcept;
    void tick(unsigned spc_cycles) noexcept;
    [[nodiscard]] std::uint8_t host_read_port(unsigned index) const noexcept;
    void host_write_port(unsigned index, std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t dsp_register(std::uint8_t index) const noexcept;
    // S-DSP accesses physical APU RAM, bypassing SPC700 I/O and IPL overlays.
    [[nodiscard]] std::uint8_t dsp_read_ram(std::uint16_t address) const noexcept {
        return ram_[address];
    }
    // Echo writeback uses the same physical RAM, without SPC700 I/O effects.
    void dsp_write_ram(std::uint16_t address, std::uint8_t value) noexcept {
        ram_[address] = value;
    }

private:
    std::array<std::uint8_t, 0x10000> ram_{};
    std::array<std::uint8_t, 0x80> dsp_{};
    std::array<std::uint8_t, 4> host_to_spc_{};
    std::array<std::uint8_t, 4> spc_to_host_{};
    std::array<std::uint8_t, 3> timer_target_{};
    std::array<std::uint8_t, 3> timer_stage2_{};
    std::array<std::uint8_t, 3> timer_output_{};
    std::array<unsigned, 3> timer_phase_{};
    std::uint8_t timer_enabled_{};
    IplRom ipl_{};
    std::uint8_t dsp_address_{};
    bool has_ipl_{};
    bool ipl_enabled_{true};
};

} // namespace gameboy
