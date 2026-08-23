#include "gameboy/cartridge.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace gameboy {
namespace {
constexpr std::size_t minimum_header_size = 0x150;
constexpr std::size_t rom_bank_size = 0x4000;
constexpr std::size_t ram_bank_size = 0x2000;
constexpr std::uint64_t rtc_day_seconds = 24 * 60 * 60;
constexpr std::uint64_t rtc_period_seconds = 512 * rtc_day_seconds;
constexpr std::array<char, 8> rtc_magic{'G', 'B', 'B', 'R', 'T', 'C', '1', 0};

std::int64_t current_unix_seconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

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
    case 0x0F:
        controller_ = Controller::mbc3;
        rtc_present_ = true;
        battery_ = true;
        break;
    case 0x10:
        controller_ = Controller::mbc3;
        rtc_present_ = true;
        ram_capacity = header_ram_size(rom_[0x149]);
        battery_ = true;
        break;
    case 0x11:
        controller_ = Controller::mbc3;
        break;
    case 0x12:
        controller_ = Controller::mbc3;
        ram_capacity = header_ram_size(rom_[0x149]);
        break;
    case 0x13:
        controller_ = Controller::mbc3;
        ram_capacity = header_ram_size(rom_[0x149]);
        battery_ = true;
        break;
    case 0x19:
        controller_ = Controller::mbc5;
        break;
    case 0x1A:
        controller_ = Controller::mbc5;
        ram_capacity = header_ram_size(rom_[0x149]);
        break;
    case 0x1B:
        controller_ = Controller::mbc5;
        ram_capacity = header_ram_size(rom_[0x149]);
        battery_ = true;
        break;
    case 0x1C:
        controller_ = Controller::mbc5;
        rumble_present_ = true;
        break;
    case 0x1D:
        controller_ = Controller::mbc5;
        ram_capacity = header_ram_size(rom_[0x149]);
        rumble_present_ = true;
        break;
    case 0x1E:
        controller_ = Controller::mbc5;
        ram_capacity = header_ram_size(rom_[0x149]);
        battery_ = true;
        rumble_present_ = true;
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
    } else if (controller_ == Controller::mbc3) {
        if (expected_rom_size > 0x400000) {
            throw std::invalid_argument("MBC3 ROM exceeds the 4 MiB MBC30 limit");
        }
        if (ram_capacity > 0x10000) {
            throw std::invalid_argument("MBC3 RAM exceeds the 64 KiB MBC30 limit");
        }
    } else if (controller_ == Controller::mbc5) {
        if (expected_rom_size > 0x800000) {
            throw std::invalid_argument("MBC5 ROM exceeds the 8 MiB address limit");
        }
        if (ram_capacity > 0x20000) {
            throw std::invalid_argument("MBC5 RAM exceeds the 128 KiB address limit");
        }
    }
    ram_.resize(ram_capacity, 0);
    if (rtc_present_) {
        rtc_last_update_ = current_unix_seconds();
        latched_rtc_ = rtc_;
    }
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
      rtc_path_(std::move(other.rtc_path_)),
      controller_(other.controller_),
      rom_bank_count_(other.rom_bank_count_),
      battery_(other.battery_),
      rtc_present_(other.rtc_present_),
      rumble_present_(other.rumble_present_),
      rumble_active_(other.rumble_active_),
      large_mbc1_rom_(other.large_mbc1_rom_),
      ram_enabled_(other.ram_enabled_),
      ram_dirty_(other.ram_dirty_),
      rom_bank_low_(other.rom_bank_low_),
      bank_upper_(other.bank_upper_),
      banking_mode_(other.banking_mode_),
      selected_rom_bank_(other.selected_rom_bank_),
      ram_rtc_select_(other.ram_rtc_select_),
      rtc_latch_value_(other.rtc_latch_value_),
      rtc_(other.rtc_),
      latched_rtc_(other.latched_rtc_),
      rtc_last_update_(other.rtc_last_update_),
      rtc_dirty_(other.rtc_dirty_) {
    other.ram_dirty_ = false;
    other.rtc_dirty_ = false;
    other.save_path_.clear();
    other.rtc_path_.clear();
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
    if (cartridge.battery_) {
        if (!cartridge.ram_.empty()) {
            cartridge.save_path_ = path;
            cartridge.save_path_.replace_extension(".sav");
            cartridge.load_battery();
        }
        if (cartridge.rtc_present_) {
            cartridge.rtc_path_ = path;
            cartridge.rtc_path_.replace_extension(".rtc");
            cartridge.load_rtc();
        }
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
        auto bank = std::size_t{1};
        if (controller_ == Controller::mbc1) {
            bank = (static_cast<std::size_t>(bank_upper_) << 5) |
                   rom_bank_low_;
        } else if (controller_ == Controller::mbc3 ||
                   controller_ == Controller::mbc5) {
            bank = selected_rom_bank_;
        }
        return read_rom_bank(bank, address - 0x4000);
    }
    if (address >= 0xA000 && address <= 0xBFFF && ram_enabled_) {
        if (controller_ == Controller::mbc3 && rtc_present_ &&
            ram_rtc_select_ >= 0x08 && ram_rtc_select_ <= 0x0C) {
            return read_rtc_register();
        }
        if (ram_.empty() ||
            (controller_ == Controller::mbc3 && ram_rtc_select_ > 0x07)) {
            return 0xFF;
        }
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

    if (controller_ == Controller::mbc3 && address <= 0x7FFF) {
        if (address <= 0x1FFF) {
            ram_enabled_ = (value & 0x0F) == 0x0A;
        } else if (address <= 0x3FFF) {
            const auto mask = rom_bank_count_ > 128 ? 0xFF : 0x7F;
            selected_rom_bank_ = static_cast<std::uint16_t>(value & mask);
            if (selected_rom_bank_ == 0) selected_rom_bank_ = 1;
        } else if (address <= 0x5FFF) {
            ram_rtc_select_ = value;
        } else {
            if (rtc_present_ && rtc_latch_value_ == 0 && value == 1) {
                latch_rtc();
            }
            rtc_latch_value_ = value;
        }
        return;
    }

    if (controller_ == Controller::mbc5 && address <= 0x7FFF) {
        if (address <= 0x1FFF) {
            ram_enabled_ = (value & 0x0F) == 0x0A;
        } else if (address <= 0x2FFF) {
            selected_rom_bank_ = static_cast<std::uint16_t>(
                (selected_rom_bank_ & 0x100) | value);
        } else if (address <= 0x3FFF) {
            selected_rom_bank_ = static_cast<std::uint16_t>(
                (selected_rom_bank_ & 0xFF) | ((value & 1) << 8));
        } else if (address <= 0x5FFF) {
            rumble_active_ = rumble_present_ && (value & 0x08) != 0;
            ram_rtc_select_ = static_cast<std::uint8_t>(
                value & (rumble_present_ ? 0x07 : 0x0F));
        }
        return;
    }

    if (address >= 0xA000 && address <= 0xBFFF && ram_enabled_) {
        if (controller_ == Controller::mbc3 && rtc_present_ &&
            ram_rtc_select_ >= 0x08 && ram_rtc_select_ <= 0x0C) {
            write_rtc_register(value);
            return;
        }
        if (ram_.empty() ||
            (controller_ == Controller::mbc3 && ram_rtc_select_ > 0x07)) {
            return;
        }
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

bool Cartridge::has_rumble() const noexcept { return rumble_present_; }

bool Cartridge::rumble_active() const noexcept { return rumble_active_; }

std::uint64_t Cartridge::rom_fingerprint() const noexcept {
    auto hash = UINT64_C(14695981039346656037);
    for (const auto byte : rom_) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void Cartridge::flush_battery() {
    if (!battery_) return;

    if (ram_dirty_ && !save_path_.empty() && !ram_.empty()) {
        std::ofstream output(save_path_, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Could not open save file: " +
                                     save_path_.string());
        }
        output.write(reinterpret_cast<const char*>(ram_.data()),
                     static_cast<std::streamsize>(ram_.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("Could not write save file: " +
                                     save_path_.string());
        }
        ram_dirty_ = false;
    }
    if (rtc_present_) flush_rtc();
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
    if (controller_ == Controller::mbc3 || controller_ == Controller::mbc5) {
        return ram_rtc_select_;
    }
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

void Cartridge::update_rtc() const noexcept {
    if (!rtc_present_) return;

    const auto now = current_unix_seconds();
    if (rtc_last_update_ == 0 || now <= rtc_last_update_) {
        rtc_last_update_ = now;
        return;
    }
    if ((rtc_[4] & 0x40) != 0) {
        rtc_last_update_ = now;
        return;
    }

    const auto elapsed = static_cast<std::uint64_t>(now - rtc_last_update_);
    rtc_last_update_ = now;
    const auto days = static_cast<std::uint16_t>(rtc_[3]) |
                      (static_cast<std::uint16_t>(rtc_[4] & 1) << 8);
    auto total = static_cast<std::uint64_t>(rtc_[0]) +
                 60 * static_cast<std::uint64_t>(rtc_[1]) +
                 3600 * static_cast<std::uint64_t>(rtc_[2]) +
                 rtc_day_seconds * days + elapsed;
    auto high = static_cast<std::uint8_t>(rtc_[4] & 0xC0);
    if (total >= rtc_period_seconds) high |= 0x80;
    total %= rtc_period_seconds;
    const auto new_days = static_cast<std::uint16_t>(total / rtc_day_seconds);
    auto remainder = total % rtc_day_seconds;
    rtc_[2] = static_cast<std::uint8_t>(remainder / 3600);
    remainder %= 3600;
    rtc_[1] = static_cast<std::uint8_t>(remainder / 60);
    rtc_[0] = static_cast<std::uint8_t>(remainder % 60);
    rtc_[3] = static_cast<std::uint8_t>(new_days);
    rtc_[4] = static_cast<std::uint8_t>(high | ((new_days >> 8) & 1));
    rtc_dirty_ = true;
}

void Cartridge::latch_rtc() noexcept {
    update_rtc();
    latched_rtc_ = rtc_;
}

std::uint8_t Cartridge::read_rtc_register() const noexcept {
    return latched_rtc_[ram_rtc_select_ - 0x08];
}

void Cartridge::write_rtc_register(const std::uint8_t value) noexcept {
    update_rtc();
    const auto index = static_cast<std::size_t>(ram_rtc_select_ - 0x08);
    switch (index) {
    case 0: rtc_[index] = static_cast<std::uint8_t>(value % 60); break;
    case 1: rtc_[index] = static_cast<std::uint8_t>(value % 60); break;
    case 2: rtc_[index] = static_cast<std::uint8_t>(value % 24); break;
    case 3: rtc_[index] = value; break;
    case 4: rtc_[index] = static_cast<std::uint8_t>(value & 0xC1); break;
    default: return;
    }
    rtc_last_update_ = current_unix_seconds();
    rtc_dirty_ = true;
}

void Cartridge::load_rtc() {
    if (!std::filesystem::exists(rtc_path_)) return;

    std::ifstream input(rtc_path_, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open RTC file: " + rtc_path_.string());
    }
    std::array<char, 8> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    input.read(reinterpret_cast<char*>(rtc_.data()),
               static_cast<std::streamsize>(rtc_.size()));
    std::array<std::uint8_t, 8> timestamp{};
    input.read(reinterpret_cast<char*>(timestamp.data()),
               static_cast<std::streamsize>(timestamp.size()));
    if (!input || magic != rtc_magic) {
        throw std::runtime_error("Invalid RTC file: " + rtc_path_.string());
    }
    std::uint64_t encoded_time = 0;
    for (std::size_t index = 0; index < timestamp.size(); ++index) {
        encoded_time |= static_cast<std::uint64_t>(timestamp[index]) <<
                        (index * 8);
    }
    rtc_last_update_ = static_cast<std::int64_t>(encoded_time);
    rtc_[0] %= 60;
    rtc_[1] %= 60;
    rtc_[2] %= 24;
    rtc_[4] &= 0xC1;
    update_rtc();
    latched_rtc_ = rtc_;
    rtc_dirty_ = false;
}

void Cartridge::flush_rtc() {
    if (!rtc_present_ || rtc_path_.empty()) return;
    update_rtc();
    if (!rtc_dirty_) return;

    std::ofstream output(rtc_path_, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Could not open RTC file: " + rtc_path_.string());
    }
    output.write(rtc_magic.data(), static_cast<std::streamsize>(rtc_magic.size()));
    output.write(reinterpret_cast<const char*>(rtc_.data()),
                 static_cast<std::streamsize>(rtc_.size()));
    const auto encoded_time = static_cast<std::uint64_t>(rtc_last_update_);
    std::array<std::uint8_t, 8> timestamp{};
    for (std::size_t index = 0; index < timestamp.size(); ++index) {
        timestamp[index] = static_cast<std::uint8_t>(encoded_time >> (index * 8));
    }
    output.write(reinterpret_cast<const char*>(timestamp.data()),
                 static_cast<std::streamsize>(timestamp.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("Could not write RTC file: " + rtc_path_.string());
    }
    rtc_dirty_ = false;
}

} // namespace gameboy
