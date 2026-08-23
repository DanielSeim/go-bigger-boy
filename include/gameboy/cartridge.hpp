#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gameboy {

class SaveStateCodec;

class Cartridge {
public:
    explicit Cartridge(std::vector<std::uint8_t> rom);
    ~Cartridge();

    Cartridge(const Cartridge&) = delete;
    Cartridge& operator=(const Cartridge&) = delete;
    Cartridge(Cartridge&& other) noexcept;
    Cartridge& operator=(Cartridge&&) = delete;

    static Cartridge from_file(const std::filesystem::path& path);

    [[nodiscard]] std::uint8_t read(std::uint16_t address) const noexcept;
    void write(std::uint16_t address, std::uint8_t value) noexcept;

    [[nodiscard]] std::string title() const;
    [[nodiscard]] std::uint8_t type() const noexcept;
    [[nodiscard]] std::size_t rom_size() const noexcept;
    [[nodiscard]] std::size_t ram_size() const noexcept;
    [[nodiscard]] bool has_battery() const noexcept;
    [[nodiscard]] bool has_rumble() const noexcept;
    [[nodiscard]] bool rumble_active() const noexcept;
    [[nodiscard]] std::uint64_t rom_fingerprint() const noexcept;
    void flush_battery();

private:
    friend class SaveStateCodec;

    enum class Controller {
        rom_only,
        mbc1,
        mbc3,
        mbc5,
    };

    [[nodiscard]] std::uint8_t read_rom_bank(std::size_t bank,
                                             std::size_t offset) const noexcept;
    [[nodiscard]] std::size_t selected_ram_bank() const noexcept;
    [[nodiscard]] std::uint8_t read_rtc_register() const noexcept;
    void write_rtc_register(std::uint8_t value) noexcept;
    void update_rtc() const noexcept;
    void latch_rtc() noexcept;
    void load_battery();
    void load_rtc();
    void flush_rtc();

    std::vector<std::uint8_t> rom_;
    std::vector<std::uint8_t> ram_;
    std::filesystem::path save_path_;
    std::filesystem::path rtc_path_;
    Controller controller_ = Controller::rom_only;
    std::size_t rom_bank_count_{};
    bool battery_{};
    bool rtc_present_{};
    bool rumble_present_{};
    bool rumble_active_{};
    bool large_mbc1_rom_{};
    bool ram_enabled_{};
    bool ram_dirty_{};
    std::uint8_t rom_bank_low_ = 1;
    std::uint8_t bank_upper_{};
    std::uint8_t banking_mode_{};
    std::uint16_t selected_rom_bank_ = 1;
    std::uint8_t ram_rtc_select_{};
    std::uint8_t rtc_latch_value_ = 0xFF;
    mutable std::array<std::uint8_t, 5> rtc_{};
    std::array<std::uint8_t, 5> latched_rtc_{};
    mutable std::int64_t rtc_last_update_{};
    mutable bool rtc_dirty_{};
};

} // namespace gameboy
