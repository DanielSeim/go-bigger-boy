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
constexpr std::size_t camera_image_size = 16 * 14 * 16;
constexpr std::uint64_t rtc_day_seconds = 24 * 60 * 60;
constexpr std::uint64_t rtc_period_seconds = 512 * rtc_day_seconds;
constexpr std::array<char, 8> rtc_magic{'G', 'B', 'B', 'R', 'T', 'C', '1', 0};
constexpr std::array<std::uint8_t, 48> nintendo_logo{
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
    0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
};

// CGB boot ROM DMG-colorization lookup data. See gbdev.io/pandocs/
// Power_Up_Sequence.html#compatibility-palettes.
constexpr std::array<std::uint8_t, 94> title_checksums{
    0x00, 0x88, 0x16, 0x36, 0xD1, 0xDB, 0xF2, 0x3C, 0x8C,
    0x92, 0x3D, 0x5C, 0x58, 0xC9, 0x3E, 0x70, 0x1D, 0x59, 0x69, 0x19,
    0x35, 0xA8, 0x14, 0xAA, 0x75, 0x95, 0x99, 0x34, 0x6F, 0x15, 0xFF,
    0x97, 0x4B, 0x90, 0x17, 0x10, 0x39, 0xF7, 0xF6, 0xA2, 0x49, 0x4E,
    0x43, 0x68, 0xE0, 0x8B, 0xF0, 0xCE, 0x0C, 0x29, 0xE8, 0xB7, 0x86,
    0x9A, 0x52, 0x01, 0x9D, 0x71, 0x9C, 0xBD, 0x5D, 0x6D, 0x67, 0x3F,
    0x6B, 0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27, 0x61, 0x18, 0x66,
    0x6A, 0xBF, 0x0D, 0xF4, 0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27,
    0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4, 0xB3,
};

constexpr std::array<std::uint8_t, 94> palette_per_checksum{
    0, 4, 5, 35, 34, 3, 31, 15, 10, 5, 19, 36, 0x87, 37,
    30, 44, 21, 32, 31, 20, 5, 33, 13, 14, 5, 29, 5, 18, 9, 3, 2, 26,
    25, 25, 41, 42, 26, 45, 42, 45, 36, 38, 0x9A, 42, 30, 41, 34, 34,
    5, 42, 6, 5, 33, 25, 42, 42, 40, 2, 16, 25, 42, 42, 5, 0, 39, 36,
    22, 25, 6, 32, 12, 36, 11, 39, 18, 39, 24, 31, 50, 17, 46, 6, 27,
    0, 47, 41, 41, 0, 0, 19, 34, 23, 18, 29,
};

constexpr std::array duplicate_title_letters{
    'B', 'E', 'F', 'A', 'A', 'R', 'B', 'E', 'K', 'E', 'K', ' ', 'R', '-',
    'U', 'R', 'A', 'R', ' ', 'I', 'N', 'A', 'I', 'L', 'I', 'C', 'E', ' ', 'R',
};

constexpr std::size_t first_duplicate_checksum = 65;
static_assert(title_checksums.size() == palette_per_checksum.size());
static_assert(title_checksums.size() - first_duplicate_checksum ==
              duplicate_title_letters.size());

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
    case 0x05:
        controller_ = Controller::mbc2;
        ram_capacity = 0x200;
        break;
    case 0x06:
        controller_ = Controller::mbc2;
        ram_capacity = 0x200;
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
    case 0xFC:
        controller_ = Controller::camera;
        ram_capacity = header_ram_size(rom_[0x149]);
        battery_ = true;
        camera_frame_.resize(camera_width * camera_height);
        camera_image_.resize(camera_image_size);
        for (std::size_t y = 0; y < camera_height; ++y) {
            for (std::size_t x = 0; x < camera_width; ++x) {
                camera_frame_[y * camera_width + x] =
                    static_cast<std::uint8_t>((x + y) & 0xFF);
            }
        }
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
        if (expected_rom_size == 0x100000) {
            constexpr std::size_t logo_offset = 0x0104;
            constexpr std::size_t sub_rom_size = 0x40000;
            auto valid_sub_headers = 0U;
            for (auto offset = std::size_t{0}; offset < expected_rom_size;
                 offset += sub_rom_size) {
                valid_sub_headers += std::equal(
                    nintendo_logo.begin(), nintendo_logo.end(),
                    rom_.begin() + offset + logo_offset);
            }
            mbc1_multicart_ = valid_sub_headers >= 2;
        }
    } else if (controller_ == Controller::mbc2) {
        if (expected_rom_size > 0x40000) {
            throw std::invalid_argument("MBC2 ROM exceeds the 256 KiB address limit");
        }
    } else if (controller_ == Controller::mbc3) {
        if (expected_rom_size > 0x400000) {
            throw std::invalid_argument("MBC3 ROM exceeds the 4 MiB MBC30 limit");
        }
        if (ram_capacity > 0x10000) {
            throw std::invalid_argument("MBC3 RAM exceeds the 64 KiB MBC30 limit");
        }
    } else if (controller_ == Controller::mbc5 ||
               controller_ == Controller::camera) {
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
      mbc1_multicart_(other.mbc1_multicart_),
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
      rtc_dirty_(other.rtc_dirty_),
      camera_registers_mapped_(other.camera_registers_mapped_),
      camera_registers_(other.camera_registers_),
      camera_frame_(std::move(other.camera_frame_)),
      camera_image_(std::move(other.camera_image_)) {
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
                              ? static_cast<std::size_t>(bank_upper_)
                                    << (mbc1_multicart_ ? 4 : 5)
                              : 0;
        return read_rom_bank(bank, address);
    }
    if (address <= 0x7FFF) {
        auto bank = std::size_t{1};
        if (controller_ == Controller::mbc1) {
            bank = (static_cast<std::size_t>(bank_upper_)
                    << (mbc1_multicart_ ? 4 : 5)) |
                   (rom_bank_low_ & (mbc1_multicart_ ? 0x0F : 0x1F));
        } else if (controller_ == Controller::mbc2 ||
                   controller_ == Controller::mbc3 ||
                   controller_ == Controller::mbc5 ||
                   controller_ == Controller::camera) {
            bank = selected_rom_bank_;
        }
        return read_rom_bank(bank, address - 0x4000);
    }
    if (address >= 0xA000 && address <= 0xBFFF && ram_enabled_) {
        if (controller_ == Controller::camera) {
            if (camera_registers_mapped_) {
                return read_camera_register(address);
            }
            if (ram_rtc_select_ == 0 && address >= 0xA100 &&
                address <= 0xAEFF && !camera_image_.empty()) {
                return camera_image_[address - 0xA100];
            }
        }
        if (controller_ == Controller::mbc2) {
            return static_cast<std::uint8_t>(
                0xF0 | ram_[(address - 0xA000) & 0x01FF]);
        }
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
            // MBC1M ignores bit 4 at the ROM pins, but the controller's
            // forbidden-bank remap still considers the full five-bit write.
            rom_bank_low_ = static_cast<std::uint8_t>(value & 0x1F);
            if (rom_bank_low_ == 0) rom_bank_low_ = 1;
        } else if (address <= 0x5FFF) {
            bank_upper_ = static_cast<std::uint8_t>(value & 0x03);
        } else {
            banking_mode_ = static_cast<std::uint8_t>(value & 0x01);
        }
        return;
    }

    if (controller_ == Controller::mbc2 && address <= 0x3FFF) {
        if ((address & 0x0100) == 0) {
            ram_enabled_ = (value & 0x0F) == 0x0A;
        } else {
            selected_rom_bank_ = static_cast<std::uint8_t>(value & 0x0F);
            if (selected_rom_bank_ == 0) selected_rom_bank_ = 1;
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

    if ((controller_ == Controller::mbc5 ||
         controller_ == Controller::camera) && address <= 0x7FFF) {
        if (address <= 0x1FFF) {
            ram_enabled_ = (value & 0x0F) == 0x0A;
        } else if (address <= 0x2FFF) {
            selected_rom_bank_ = static_cast<std::uint16_t>(
                (selected_rom_bank_ & 0x100) | value);
        } else if (address <= 0x3FFF) {
            selected_rom_bank_ = static_cast<std::uint16_t>(
                (selected_rom_bank_ & 0xFF) | ((value & 1) << 8));
        } else if (address <= 0x5FFF) {
            if (controller_ == Controller::camera) {
                camera_registers_mapped_ = (value & 0x10) != 0;
                ram_rtc_select_ = static_cast<std::uint8_t>(value & 0x0F);
            } else {
                rumble_active_ = rumble_present_ && (value & 0x08) != 0;
                ram_rtc_select_ = static_cast<std::uint8_t>(
                    value & (rumble_present_ ? 0x07 : 0x0F));
            }
        }
        return;
    }

    if (address >= 0xA000 && address <= 0xBFFF && ram_enabled_) {
        if (controller_ == Controller::camera && camera_registers_mapped_) {
            write_camera_register(address, value);
            return;
        }
        if (controller_ == Controller::mbc2) {
            const auto index = static_cast<std::size_t>(
                (address - 0xA000) & 0x01FF);
            const auto nibble = static_cast<std::uint8_t>(value & 0x0F);
            if (ram_[index] != nibble) {
                ram_[index] = nibble;
                ram_dirty_ = true;
            }
            return;
        }
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
    const auto title_end = supports_cgb() ? std::size_t{0x143}
                                          : std::size_t{0x144};
    const auto end = std::find(rom_.begin() + title_begin,
                               rom_.begin() + title_end, 0);
    return {rom_.begin() + title_begin, end};
}

std::uint8_t Cartridge::type() const noexcept { return rom_[0x147]; }

bool Cartridge::supports_cgb() const noexcept {
    return (rom_[0x143] & 0x80) != 0;
}

bool Cartridge::requires_cgb() const noexcept {
    return (rom_[0x143] & 0xC0) == 0xC0;
}

std::uint8_t Cartridge::cgb_compatibility_palette_id() const noexcept {
    const auto old_licensee = rom_[0x14B];
    const auto is_nintendo = old_licensee == 0x01 ||
                             (old_licensee == 0x33 && rom_[0x144] == '0' &&
                              rom_[0x145] == '1');
    if (!is_nintendo) return 0;

    std::uint8_t checksum = 0;
    for (std::size_t address = 0x134; address <= 0x143; ++address) {
        checksum = static_cast<std::uint8_t>(checksum + rom_[address]);
    }
    for (std::size_t index = 0; index < title_checksums.size(); ++index) {
        if (title_checksums[index] != checksum) continue;
        if (index >= first_duplicate_checksum &&
            rom_[0x137] != duplicate_title_letters[index -
                                                    first_duplicate_checksum]) {
            continue;
        }
        return static_cast<std::uint8_t>(palette_per_checksum[index] & 0x7F);
    }
    return 0;
}

std::size_t Cartridge::rom_size() const noexcept { return rom_.size(); }

std::size_t Cartridge::ram_size() const noexcept { return ram_.size(); }

bool Cartridge::has_battery() const noexcept { return battery_; }

bool Cartridge::has_rtc() const noexcept { return rtc_present_; }

bool Cartridge::has_rumble() const noexcept { return rumble_present_; }

bool Cartridge::rumble_active() const noexcept { return rumble_active_; }

bool Cartridge::has_camera() const noexcept {
    return controller_ == Controller::camera;
}

void Cartridge::set_camera_frame(const std::uint8_t* grayscale,
                                 const std::size_t size) noexcept {
    if (!has_camera() || grayscale == nullptr ||
        size != camera_width * camera_height) {
        return;
    }
    std::copy_n(grayscale, size, camera_frame_.begin());
}

std::uint64_t Cartridge::rom_fingerprint() const noexcept {
    auto hash = UINT64_C(14695981039346656037);
    for (const auto byte : rom_) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::vector<std::uint8_t> Cartridge::export_battery_ram() const {
    return battery_ ? ram_ : std::vector<std::uint8_t>{};
}

void Cartridge::import_battery_ram(const std::vector<std::uint8_t>& data) {
    if (!battery_ || ram_.empty()) {
        throw std::invalid_argument(
            "Cartridge has no battery-backed save RAM");
    }
    if (data.size() != ram_.size()) {
        throw std::invalid_argument(
            "Save RAM size does not match the loaded cartridge");
    }
    ram_ = data;
    ram_dirty_ = true;
}

std::vector<std::uint8_t> Cartridge::export_rtc_data() const {
    if (!rtc_present_) return {};
    update_rtc();

    std::vector<std::uint8_t> data;
    data.reserve(rtc_magic.size() + rtc_.size() + sizeof(std::uint64_t));
    for (const auto byte : rtc_magic) {
        data.push_back(static_cast<std::uint8_t>(byte));
    }
    data.insert(data.end(), rtc_.begin(), rtc_.end());
    const auto timestamp = static_cast<std::uint64_t>(rtc_last_update_);
    for (unsigned byte = 0; byte < sizeof(timestamp); ++byte) {
        data.push_back(static_cast<std::uint8_t>(timestamp >> (byte * 8)));
    }
    return data;
}

void Cartridge::import_rtc_data(const std::vector<std::uint8_t>& data) {
    constexpr auto rtc_data_size = rtc_magic.size() + 5 + sizeof(std::uint64_t);
    if (!rtc_present_) {
        throw std::invalid_argument("Cartridge has no real-time clock");
    }
    if (data.size() != rtc_data_size ||
        !std::equal(rtc_magic.begin(), rtc_magic.end(), data.begin())) {
        throw std::invalid_argument("Invalid RTC data");
    }

    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(rtc_magic.size()),
                rtc_.size(), rtc_.begin());
    std::uint64_t timestamp = 0;
    const auto timestamp_offset = rtc_magic.size() + rtc_.size();
    for (unsigned byte = 0; byte < sizeof(timestamp); ++byte) {
        timestamp |= static_cast<std::uint64_t>(data[timestamp_offset + byte])
                     << (byte * 8);
    }
    rtc_last_update_ = static_cast<std::int64_t>(timestamp);
    rtc_[0] %= 60;
    rtc_[1] %= 60;
    rtc_[2] %= 24;
    rtc_[4] &= 0xC1;
    update_rtc();
    latched_rtc_ = rtc_;
    rtc_dirty_ = true;
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
    if (controller_ == Controller::mbc3 || controller_ == Controller::mbc5 ||
        controller_ == Controller::camera) {
        return ram_rtc_select_;
    }
    return controller_ == Controller::mbc1 && !large_mbc1_rom_ &&
                   banking_mode_ != 0
               ? bank_upper_
               : 0;
}

std::uint8_t Cartridge::read_camera_register(
    const std::uint16_t address) const noexcept {
    const auto index = static_cast<std::size_t>((address - 0xA000) & 0x7F);
    return index == 0 ? camera_registers_[0] : 0;
}

void Cartridge::write_camera_register(const std::uint16_t address,
                                      const std::uint8_t value) noexcept {
    const auto index = static_cast<std::size_t>((address - 0xA000) & 0x7F);
    if (index >= camera_registers_.size()) return;
    if (index == 0) {
        const auto was_capturing = (camera_registers_[0] & 1) != 0;
        camera_registers_[0] = static_cast<std::uint8_t>(value & 0x07);
        if (!was_capturing && (value & 1) != 0) capture_camera_image();
        return;
    }
    camera_registers_[index] = value;
}

void Cartridge::capture_camera_image() noexcept {
    if (camera_frame_.size() != camera_width * camera_height ||
        camera_image_.size() != camera_image_size) {
        camera_registers_[0] &= 0x06;
        return;
    }

    const auto exposure = static_cast<unsigned>(camera_registers_[2]) << 8 |
                          camera_registers_[3];
    constexpr std::array<double, 32> gain{
        0.8809390, 0.9149149, 0.9457498, 0.9739758,
        1.0000000, 1.0241412, 1.0466537, 1.0677433,
        1.0875793, 1.1240310, 1.1568911, 1.1868043,
        1.2142561, 1.2396208, 1.2743837, 1.3157323,
        1.3525190, 1.3856512, 1.4157897, 1.4434309,
        1.4689574, 1.4926697, 1.5148087, 1.5355703,
        1.5551159, 1.5735801, 1.5910762, 1.6077008,
        1.6235366, 1.6386550, 1.6531183, 1.6669808,
    };
    const auto gain_value = gain[camera_registers_[1] & 0x1F];
    const auto processed_luminance = [&](int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(camera_width) - 1);
        y = std::clamp(y, 0, static_cast<int>(camera_height) - 1);
        return static_cast<double>(
                   camera_frame_[static_cast<std::size_t>(y) * camera_width +
                                 static_cast<std::size_t>(x)]) *
               gain_value * static_cast<double>(exposure) / 0x1000;
    };
    constexpr std::array<double, 8> edge_ratios{
        0.5, 0.75, 1.0, 1.25, 2.0, 3.0, 4.0, 5.0,
    };
    const auto enhance_edges = (camera_registers_[1] & 0xE0) == 0xE0;
    const auto edge_ratio = edge_ratios[(camera_registers_[4] >> 4) & 7];
    std::fill(camera_image_.begin(), camera_image_.end(), 0);

    for (std::size_t y = 0; y < camera_height; ++y) {
        for (std::size_t x = 0; x < camera_width; ++x) {
            auto luminance = processed_luminance(static_cast<int>(x),
                                                 static_cast<int>(y));
            if (enhance_edges) {
                luminance += luminance * 4 * edge_ratio;
                luminance -= processed_luminance(static_cast<int>(x) - 1,
                                                  static_cast<int>(y)) *
                             edge_ratio;
                luminance -= processed_luminance(static_cast<int>(x) + 1,
                                                  static_cast<int>(y)) *
                             edge_ratio;
                luminance -= processed_luminance(static_cast<int>(x),
                                                  static_cast<int>(y) - 1) *
                             edge_ratio;
                luminance -= processed_luminance(static_cast<int>(x),
                                                  static_cast<int>(y) + 1) *
                             edge_ratio;
            }
            const auto matrix = ((x & 3) + (y & 3) * 4) * 3 + 6;
            const auto t0 = camera_registers_[matrix];
            const auto t1 = camera_registers_[matrix + 1];
            const auto t2 = camera_registers_[matrix + 2];
            std::uint8_t shade = 0;
            if (luminance < t0) shade = 3;
            else if (luminance < t1) shade = 2;
            else if (luminance < t2) shade = 1;

            const auto tile = (y / 8) * 16 + x / 8;
            const auto row = y & 7;
            const auto offset = tile * 16 + row * 2;
            const auto bit = static_cast<std::uint8_t>(0x80 >> (x & 7));
            if ((shade & 1) != 0) camera_image_[offset] |= bit;
            if ((shade & 2) != 0) camera_image_[offset + 1] |= bit;
        }
    }
    camera_registers_[0] &= 0x06;
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
