#include "gameboy/snes_audio_host.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace gameboy {

SgbProgramRom::Error SgbProgramRom::validate(
    const std::vector<std::uint8_t>& bytes) noexcept {
    // The known SGB1 and SGB2 program images are 256 KiB and 512 KiB LoROM.
    if (bytes.size() != 0x40000 && bytes.size() != 0x80000) {
        return Error::unsupported_size;
    }
    if ((bytes[0x7FD5] & 0xEFU) != 0x20U) {
        return Error::unsupported_mapping;
    }
    const auto vector = static_cast<std::uint16_t>(
        bytes[0x7FFC] | (static_cast<unsigned>(bytes[0x7FFD]) << 8));
    if (vector < 0x8000) return Error::invalid_reset_vector;
    return Error::none;
}

SgbProgramRom SgbProgramRom::from_file(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size != 0x40000 && size != 0x80000) {
        throw std::invalid_argument("unsupported SGB program ROM size");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open SGB program ROM");
    std::vector<std::uint8_t> bytes(
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    if (input.bad() || bytes.size() != size) {
        throw std::runtime_error("could not read complete SGB program ROM");
    }
    return SgbProgramRom(std::move(bytes));
}

SgbProgramRom::SgbProgramRom(std::vector<std::uint8_t> bytes)
    : bytes_(std::move(bytes)) {
    if (validate(bytes_) != Error::none) {
        throw std::invalid_argument("unsupported SGB program ROM format");
    }
}

std::uint8_t SgbProgramRom::read(const std::uint8_t bank,
                                 const std::uint16_t address) const noexcept {
    // Only the high half of the LoROM bank is mapped here. The future SNES
    // host owns RAM, I/O and all lower-half mapping.
    if (address < 0x8000 || bank == 0x7E || bank == 0x7F) return 0xFF;
    const auto offset =
        ((static_cast<std::size_t>(bank & 0x7FU) * 0x8000) |
         (address & 0x7FFFU)) % bytes_.size();
    return bytes_[offset];
}

std::uint16_t SgbProgramRom::reset_vector() const noexcept {
    return static_cast<std::uint16_t>(
        bytes_[0x7FFC] | (static_cast<unsigned>(bytes_[0x7FFD]) << 8));
}

void SnesApuBus::reset() noexcept {
    ram_.fill(0);
    dsp_.fill(0);
    host_to_spc_.fill(0);
    spc_to_host_.fill(0);
    timer_target_.fill(0);
    timer_stage2_.fill(0);
    timer_output_.fill(0);
    timer_phase_.fill(0);
    timer_enabled_ = 0;
    dsp_address_ = 0;
    ipl_enabled_ = true;
    dsp_write_observer_ = nullptr;
    dsp_write_context_ = nullptr;
    ram_write_observer_ = nullptr;
    ram_write_context_ = nullptr;
}

void SnesApuBus::install_ipl(const IplRom& image) noexcept {
    ipl_ = image;
    has_ipl_ = true;
    ipl_enabled_ = true;
}

std::uint8_t SnesApuBus::spc_read(const std::uint16_t address) noexcept {
    if (address >= 0xFFC0 && ipl_enabled_ && has_ipl_) {
        return ipl_[address - 0xFFC0];
    }
    if (address >= 0xF4 && address <= 0xF7) {
        return host_to_spc_[address - 0xF4];
    }
    if (address == 0xF2) return dsp_address_;
    if (address == 0xF3) return dsp_[dsp_address_ & 0x7FU];
    if (address >= 0xFD && address <= 0xFF) {
        auto& output = timer_output_[address - 0xFD];
        const auto value = output;
        output = 0;
        return value;
    }
    if (address == 0xF0 || address == 0xF1 ||
        (address >= 0xFA && address <= 0xFC)) return 0;
    return ram_[address];
}

void SnesApuBus::spc_write(const std::uint16_t address,
                           const std::uint8_t value) noexcept {
    if (address >= 0xF4 && address <= 0xF7) {
        spc_to_host_[address - 0xF4] = value;
        ram_[address] = value;
        if (ram_write_observer_)
            ram_write_observer_(ram_write_context_, address, value);
        return;
    }
    if (address == 0xF1) {
        if ((value & 0x10U) != 0) {
            host_to_spc_[0] = 0;
            host_to_spc_[1] = 0;
        }
        if ((value & 0x20U) != 0) {
            host_to_spc_[2] = 0;
            host_to_spc_[3] = 0;
        }
        ipl_enabled_ = (value & 0x80U) != 0;
        const auto enabled = static_cast<std::uint8_t>(value & 0x07U);
        for (unsigned index = 0; index < 3; ++index) {
            const auto bit = static_cast<std::uint8_t>(1U << index);
            if ((enabled & bit) != 0 && (timer_enabled_ & bit) == 0) {
                timer_stage2_[index] = 0;
                timer_output_[index] = 0;
            }
        }
        timer_enabled_ = enabled;
    } else if (address == 0xF2) {
        dsp_address_ = value;
    } else if (address == 0xF3) {
        if ((dsp_address_ & 0x80U) == 0) {
            dsp_[dsp_address_] = value;
            if (dsp_write_observer_) {
                dsp_write_observer_(dsp_write_context_, dsp_address_, value);
            }
        }
    } else if (address >= 0xFA && address <= 0xFC) {
        timer_target_[address - 0xFA] = value;
    }
    // Physical RAM remains writable under all I/O and IPL overlays.
    ram_[address] = value;
    if (ram_write_observer_)
        ram_write_observer_(ram_write_context_, address, value);
}

void SnesApuBus::tick(const unsigned spc_cycles) noexcept {
    for (unsigned index = 0; index < 3; ++index) {
        const unsigned period = index == 2 ? 16 : 128;
        // Keep stage 1 running even while disabled. Stage 2 and 3 start only
        // when their enable bit is set in CONTROL.
        const auto elapsed = static_cast<std::uint64_t>(timer_phase_[index]) +
                             spc_cycles;
        const auto ticks = elapsed / period;
        timer_phase_[index] = static_cast<unsigned>(elapsed % period);
        if ((timer_enabled_ & (1U << index)) == 0 || ticks == 0) continue;

        const auto target = timer_target_[index];
        const auto stage2 = timer_stage2_[index];
        // The comparison happens *after* increment. A target of zero means
        // 256 ticks, including when a running timer's target was changed.
        auto distance = (static_cast<unsigned>(target) + 256U - stage2) & 0xFFU;
        if (distance == 0) distance = 256;
        if (ticks < distance) {
            timer_stage2_[index] = static_cast<std::uint8_t>(stage2 + ticks);
            continue;
        }
        const auto interval = target == 0 ? 256U : target;
        const auto after_first = ticks - distance;
        const auto output_ticks = 1U + after_first / interval;
        timer_stage2_[index] =
            static_cast<std::uint8_t>(after_first % interval);
        timer_output_[index] = static_cast<std::uint8_t>(
            (timer_output_[index] + output_ticks) & 0x0FU);
    }
}

std::uint8_t SnesApuBus::host_read_port(const unsigned index) const noexcept {
    return index < spc_to_host_.size() ? spc_to_host_[index] : 0xFF;
}

void SnesApuBus::host_write_port(const unsigned index,
                                 const std::uint8_t value) noexcept {
    if (index < host_to_spc_.size()) host_to_spc_[index] = value;
}

std::uint8_t SnesApuBus::dsp_register(const std::uint8_t index) const noexcept {
    return dsp_[index & 0x7FU];
}

} // namespace gameboy
