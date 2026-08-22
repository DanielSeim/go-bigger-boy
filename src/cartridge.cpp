#include "gameboy/cartridge.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace gameboy {
namespace {
constexpr std::size_t minimum_header_size = 0x150;
constexpr std::size_t rom_bank_size = 0x4000;
constexpr std::size_t ram_bank_size = 0x2000;

std::size_t header_rom_size(const std::uint8_t code) {
    if (code <= 0x08) {
        return std::size_t{0x8000} << code;
    }
    switch (code) {
    case 0x52: return 72 * rom_bank_size;
    case 0x53: return 80 * rom_bank_size;
    case 0x54: return 96 * rom_bank_size;
    default: throw std::invalid_argument("Unsupported ROM size code in cartridge header");
    }
}

std::size_t header_ram_size(const std::uint8_t code) {
    switch (code) {
    case 0x00: return 0;
    case 0x01: return 0; // Listed historically, but unused by real cartridges.
    case 0x02: return 0x2000;
    case 0x03: return 0x8000;
    case 0x04: return 0x20000;
    case 0x05: return 0x10000;
    default: throw std::invalid_argument("Unsupported RAM size code in cartridge header");
    }
}
} // namespace

Cartridge::Cartridge(std::vector<std::uint8_t> rom) : rom_(std::move(rom)) {
    if (rom_.size() < minimum_header_size) {
        throw std::invalid_argument("ROM is too small to contain a Game Boy header");
    }

    const auto expected_rom_size = header_rom_size(rom_[0x148]);
    if (rom_.size() < expected_rom_size) {
        throw std::invalid_argument("ROM is smaller than its header-declared size");
    }
    rom_bank_count_ = expected_rom_size / rom_bank_size;

    auto ram_capacity = std::size_t{0};
    switch (type()) {
    case 0x00:
        controller_ = Controller::rom_only;
        break;
    case 0x01:
        controller_ = Controller::mbc1;
        break;
    case 0x02:
        controller_ = Controller::mbc1;
        ram_capacity = header_ram_size(rom_[0x149]);
        break;
    case 0x03:
        controller_ = Controller::mbc1;
        ram_capacity = header_ram_size(rom_[0x149]);
        battery_ = true;
        break;
    case 0x08:
        controller_ = Controller::rom_only;
        ram_capacity = header_ram_size(rom_[0x149]);
        ram_enabled_ = true;
        break;
    case 0x09:
        controller_ = Controller::rom_only;
        ram_capacity = header_ram_size(rom_[0x149]);
        ram_enabled_ = true;
        battery_ = true;
        break;
    default:
        throw std::invalid_argument("Unsupported cartridge controller type: " +
                                    std::to_string(type()));
    }

    if (controller_ == Controller::mbc1) {
        if (expected_rom_size > 0x200000) {
            throw std::invalid_argument("MBC1 ROM exceeds the 2 MiB address limit");
        }
        if (ram_capacity > 0x8000) {
            throw std::invalid_argument("MBC1 RAM exceeds the 32 KiB address limit");
        }
        large_mbc1_rom_ = expected_rom_size > 0x80000;
    }
    ram_.resize(ram_capacity, 0);
}

Cartridge::~Cartridge() {
    try {
        flush_battery();
    } catch (...) {
        // Explicit flush_battery() reports errors; destructors cannot.
    }
}

Cartridge::Cartridge(Cartridge&& other) noexcept
    : rom_(std::move(other.rom_)),
      ram_(std::move(other.ram_)),
      save_path_(std::move(other.save_path_)),
      controller_(other.controller_),
      rom_bank_count_(other.rom_bank_count_),
      battery_(other.battery_),
      large_mbc1_rom_(other.large_mbc1_rom_),
      ram_enabled_(other.ram_enabled_),
      ram_dirty_(other.ram_dirty_),
      rom_bank_low_(other.rom_bank_low_),
      bank_upper_(other.bank_upper_),
      banking_mode_(other.banking_mode_) {
    other.ram_dirty_ = false;
    other.save_path_.clear();
}

Cartridge Cartridge::from_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open ROM: " + path.string());
    }

    std::vector<std::uint8_t> bytes(
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    if (input.bad()) {
        throw std::runtime_error("Could not read ROM: " + path.string());
    }

    Cartridge cartridge(std::move(bytes));
    if (cartridge.battery_ && !cartridge.ram_.empty()) {
        cartridge.save_path_ = path;
        cartridge.save_path_.replace_extension(".sav");
        cartridge.load_battery();
    }
    return cartridge;
}

std::uint8_t Cartridge::read(const std::uint16_t address) const noexcept {
    if (address <= 0x3FFF) {
        const auto bank = controller_ == Controller::mbc1 && banking_mode_ != 0
                              ? static_cast<std::size_t>(bank_upper_) << 5
                              : 0;
        return read_rom_bank(bank, address);
    }
    if (address <= 0x7FFF) {
        const auto bank = controller_ == Controller::mbc1
                              ? (static_cast<std::size_t>(bank_upper_) << 5) |
                                    rom_bank_low_
                              : 1;
        return read_rom_bank(bank, address - 0x4000);
    }
    if (address >= 0xA000 && address <= 0xBFFF && !ram_.empty() &&
        ram_enabled_) {
        const auto index = selected_ram_bank() * ram_bank_size +
                           (address - 0xA000);
        return ram_[index % ram_.size()];
    }
    return 0xFF;
}

void Cartridge::write(const std::uint16_t address,
                      const std::uint8_t value) noexcept {
    if (controller_ == Controller::mbc1 && address <= 0x7FFF) {
        if (address <= 0x1FFF) {
            ram_enabled_ = (value & 0x0F) == 0x0A;
        } else if (address <= 0x3FFF) {
            rom_bank_low_ = static_cast<std::uint8_t>(value & 0x1F);
            if (rom_bank_low_ == 0) rom_bank_low_ = 1;
        } else if (address <= 0x5FFF) {
            bank_upper_ = static_cast<std::uint8_t>(value & 0x03);
        } else {
            banking_mode_ = static_cast<std::uint8_t>(value & 0x01);
        }
        return;
    }

    if (address >= 0xA000 && address <= 0xBFFF && !ram_.empty() &&
        ram_enabled_) {
        const auto index = (selected_ram_bank() * ram_bank_size +
                            (address - 0xA000)) % ram_.size();
        if (ram_[index] != value) {
            ram_[index] = value;
            ram_dirty_ = true;
        }
    }
}

std::string Cartridge::title() const {
    constexpr std::size_t title_begin = 0x134;
    constexpr std::size_t title_end = 0x144;
    const auto end = std::find(rom_.begin() + title_begin,
                               rom_.begin() + title_end, 0);
    return {rom_.begin() + title_begin, end};
}

std::uint8_t Cartridge::type() const noexcept { return rom_[0x147]; }

std::size_t Cartridge::rom_size() const noexcept { return rom_.size(); }

std::size_t Cartridge::ram_size() const noexcept { return ram_.size(); }

bool Cartridge::has_battery() const noexcept { return battery_; }

void Cartridge::flush_battery() {
    if (!battery_ || !ram_dirty_ || save_path_.empty() || ram_.empty()) {
        return;
    }
    std::ofstream output(save_path_, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Could not open save file: " + save_path_.string());
    }
    output.write(reinterpret_cast<const char*>(ram_.data()),
                 static_cast<std::streamsize>(ram_.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("Could not write save file: " + save_path_.string());
    }
    ram_dirty_ = false;
}

std::uint8_t Cartridge::read_rom_bank(const std::size_t bank,
                                      const std::size_t offset) const noexcept {
    if (rom_bank_count_ == 0) {
        return 0xFF;
    }
    const auto index = (bank % rom_bank_count_) * rom_bank_size + offset;
    return index < rom_.size() ? rom_[index] : 0xFF;
}

std::size_t Cartridge::selected_ram_bank() const noexcept {
    return controller_ == Controller::mbc1 && !large_mbc1_rom_ &&
                   banking_mode_ != 0
               ? bank_upper_
               : 0;
}

void Cartridge::load_battery() {
    if (!std::filesystem::exists(save_path_)) {
        return;
    }
    std::ifstream input(save_path_, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open save file: " + save_path_.string());
    }
    input.read(reinterpret_cast<char*>(ram_.data()),
               static_cast<std::streamsize>(ram_.size()));
    if (input.bad()) {
        throw std::runtime_error("Could not read save file: " + save_path_.string());
    }
    ram_dirty_ = false;
}

} // namespace gameboy
