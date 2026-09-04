#include "gameboy/cartridge.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace gameboy {
namespace {

constexpr std::uint64_t rtc_day_seconds = 24 * 60 * 60;
constexpr std::uint64_t rtc_period_seconds = 512 * rtc_day_seconds;
constexpr std::array<char, 8> rtc_magic{'G', 'B', 'B', 'R', 'T', 'C', '1', 0};
constexpr std::array<char, 8> camera_save_magic{'G', 'B', 'B', 'C', 'A', 'M', '1', 0};

std::int64_t current_unix_seconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

void Cartridge::set_persistence_path(const std::filesystem::path& base_path) {
    if (!save_path_.empty() || !rtc_path_.empty()) flush_battery();
    save_path_.clear();
    rtc_path_.clear();
    if (!battery_ || base_path.empty()) return;
    if (!ram_.empty()) {
        save_path_ = base_path;
        save_path_.replace_extension(".sav");
        load_battery();
    }
    if (rtc_present_) {
        rtc_path_ = base_path;
        rtc_path_.replace_extension(".rtc");
        load_rtc();
    }
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

std::vector<std::uint8_t> Cartridge::export_battery_save() const {
    if (!battery_) return {};
    auto data = ram_;
    if (controller_ == Controller::camera && !camera_image_.empty()) {
        data.insert(data.end(), camera_save_magic.begin(), camera_save_magic.end());
        data.insert(data.end(), camera_image_.begin(), camera_image_.end());
    }
    return data;
}

void Cartridge::import_battery_save(const std::vector<std::uint8_t>& data) {
    if (!battery_ || ram_.empty()) {
        throw std::invalid_argument(
            "Cartridge has no battery-backed save RAM");
    }
    const auto camera_payload = camera_save_magic.size() + camera_image_.size();
    const auto has_camera_payload = controller_ == Controller::camera &&
        data.size() == ram_.size() + camera_payload &&
        std::equal(camera_save_magic.begin(), camera_save_magic.end(),
                   data.begin() + static_cast<std::ptrdiff_t>(ram_.size()));
    if (data.size() != ram_.size() && !has_camera_payload) {
        throw std::invalid_argument(
            "Save data size does not match the loaded cartridge");
    }
    std::copy_n(data.begin(), ram_.size(), ram_.begin());
    if (has_camera_payload) {
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(ram_.size()) +
                        static_cast<std::ptrdiff_t>(camera_save_magic.size()),
                    camera_image_.size(), camera_image_.begin());
    }
    ram_dirty_ = true;
    if (has_camera_payload) camera_image_dirty_ = true;
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

    const auto camera_save = controller_ == Controller::camera &&
                             !camera_image_.empty();
    if ((ram_dirty_ || camera_image_dirty_) && !save_path_.empty() &&
        !ram_.empty()) {
        std::ofstream output(save_path_, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Could not open save file: " +
                                     save_path_.string());
        }
        output.write(reinterpret_cast<const char*>(ram_.data()),
                     static_cast<std::streamsize>(ram_.size()));
        if (camera_save) {
            output.write(camera_save_magic.data(),
                         static_cast<std::streamsize>(camera_save_magic.size()));
            output.write(reinterpret_cast<const char*>(camera_image_.data()),
                         static_cast<std::streamsize>(camera_image_.size()));
        }
        output.flush();
        if (!output) {
            throw std::runtime_error("Could not write save file: " +
                                     save_path_.string());
        }
        ram_dirty_ = false;
        camera_image_dirty_ = false;
    }
    if (rtc_present_) flush_rtc();
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
    if (controller_ == Controller::camera && !camera_image_.empty()) {
        std::array<char, camera_save_magic.size()> magic{};
        input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (input && magic == camera_save_magic) {
            input.read(reinterpret_cast<char*>(camera_image_.data()),
                       static_cast<std::streamsize>(camera_image_.size()));
            if (!input) {
                throw std::runtime_error("Could not read camera image from save file: " +
                                         save_path_.string());
            }
        }
        input.clear();
    }
    ram_dirty_ = false;
    camera_image_dirty_ = false;
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

