#pragma once

#include "gameboy/link_compatibility.hpp"

#include <cstddef>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gameboy {

class SaveStateCodec;
class SaveStateCartridgeCodec;
class SaveStateBusCodec;
struct CartridgeExtraState;

class Cartridge {
public:
    explicit Cartridge(std::vector<std::uint8_t> rom);
    ~Cartridge();

    Cartridge(const Cartridge&) = delete;
    Cartridge& operator=(const Cartridge&) = delete;
    Cartridge(Cartridge&& other) noexcept;
    Cartridge& operator=(Cartridge&&) = delete;

    // MMM01 boots from the menu header in the last 32 KiB; ordinary ROMs use zero.
    [[nodiscard]] static std::size_t boot_header_offset(const std::vector<std::uint8_t>& rom) noexcept;
    static Cartridge from_file(const std::filesystem::path& path);
    void set_persistence_path(const std::filesystem::path& base_path);

    [[nodiscard]] std::uint8_t read(std::uint16_t address) const noexcept;
    void write(std::uint16_t address, std::uint8_t value) noexcept;

    [[nodiscard]] std::string title() const;
    [[nodiscard]] std::uint8_t type() const noexcept;
    // MBC5 latch (including bit 8); false for other mapper types.
    [[nodiscard]] bool debug_mbc5_rom_bank(std::uint16_t& bank) const noexcept {
        if (controller_ != Controller::mbc5) return false;
        bank = selected_rom_bank_;
        return true;
    }
    struct Mmm01Diagnostics {
        std::array<std::uint8_t,4> registers{};
        std::size_t requested_bank0{}, requested_bank1{}, bank0{}, bank1{};
        std::size_t requested_ram_bank{}, ram_bank{}, rom_banks{};
        bool mapped{}, mode_locked{}, ram_enabled{}, ram_present{}, menu_first{};
    };
    // Uses the same bank selectors and physical address wrapping as read().
    [[nodiscard]] bool debug_mmm01(Mmm01Diagnostics& state) const noexcept;
    [[nodiscard]] bool supports_cgb() const noexcept;
    // Header 0x146 advertises software that can use Super Game Boy
    // enhancements.  This is a capability flag, not a requirement: the
    // cartridge remains playable on ordinary monochrome hardware.
    [[nodiscard]] bool supports_sgb() const noexcept;
    [[nodiscard]] bool requires_cgb() const noexcept;
    [[nodiscard]] std::uint8_t cgb_compatibility_palette_id() const noexcept;
    [[nodiscard]] std::size_t rom_size() const noexcept;
    [[nodiscard]] std::size_t ram_size() const noexcept;
    [[nodiscard]] bool has_battery() const noexcept;
    [[nodiscard]] bool has_rtc() const noexcept;
    [[nodiscard]] bool has_rumble() const noexcept;
    [[nodiscard]] bool rumble_active() const noexcept;
    [[nodiscard]] bool has_camera() const noexcept;
    static constexpr std::size_t camera_width = 128;
    static constexpr std::size_t camera_height = 112;
    void set_camera_frame(const std::uint8_t* grayscale,
                          std::size_t size) noexcept;
    [[nodiscard]] std::uint64_t rom_fingerprint() const noexcept;
    // Stable link-compatibility identity. This is intentionally broader than
    // rom_fingerprint(): compatible Pokémon releases may have different
    // localized/versioned ROM bytes while sharing a link protocol.
    [[nodiscard]] std::uint64_t link_compatibility_id() const noexcept;
    [[nodiscard]] LinkCompatibilityProfile link_compatibility_profile() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> export_battery_ram() const;
    void import_battery_ram(const std::vector<std::uint8_t>& data);
    [[nodiscard]] std::vector<std::uint8_t> export_battery_save() const;
    void import_battery_save(const std::vector<std::uint8_t>& data);
    [[nodiscard]] std::vector<std::uint8_t> export_rtc_data() const;
    void import_rtc_data(const std::vector<std::uint8_t>& data);
    void flush_battery();
    using RtcClock = std::int64_t (*)(void*) noexcept;
    // nullptr restores the system clock. Host clock sources are not serialized.
    void set_rtc_clock(RtcClock clock, void* context = nullptr) noexcept;
    [[nodiscard]] bool has_accelerometer() const noexcept;
    void set_accelerometer(std::uint16_t x, std::uint16_t y) noexcept;
    [[nodiscard]] bool has_infrared() const noexcept;
    void set_infrared_input(bool light) noexcept;
    [[nodiscard]] bool infrared_output() const noexcept;
    // Independent cartridge time in normal-speed T-cycles (EEPROM programming).
    void tick(unsigned cycles) noexcept;


private:
    friend class SaveStateCodec;
    friend class SaveStateCartridgeCodec;
    friend class SaveStateBusCodec;

    enum class Controller {
        rom_only,
        mbc1,
        mbc2,
        mbc3,
        mbc5,
        camera,
        mmm01,
        mbc7,
        huc1,
        huc3,
        tama5,
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
    [[nodiscard]] std::uint8_t read_camera_register(
        std::uint16_t address) const noexcept;
    void write_camera_register(std::uint16_t address,
                               std::uint8_t value) noexcept;
    void capture_camera_image() noexcept;

    [[nodiscard]] std::uint8_t read_extra(std::uint16_t address) const noexcept;
    void write_extra(std::uint16_t address, std::uint8_t value) noexcept;
    [[nodiscard]] std::size_t extra_rom_bank(bool upper) const noexcept;
    [[nodiscard]] std::size_t extra_ram_bank() const noexcept;
    void write_eeprom(std::uint8_t value) noexcept;
    void execute_huc_command() noexcept;
    void execute_tama_command() noexcept;
    [[nodiscard]] std::int64_t rtc_now() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> export_extra_rtc() const;
    void import_extra_rtc(const std::vector<std::uint8_t>& data);
    std::unique_ptr<CartridgeExtraState> extra_;
    RtcClock rtc_clock_{};
    void* rtc_clock_context_{};

    std::vector<std::uint8_t> rom_;
    std::uint64_t rom_fingerprint_{};
    bool mmm01_menu_first_{};
    std::vector<std::uint8_t> ram_;
    std::filesystem::path save_path_;
    std::filesystem::path rtc_path_;
    Controller controller_ = Controller::rom_only;
    std::size_t rom_bank_count_{};
    std::size_t header_offset_{};
    bool battery_{};
    bool rtc_present_{};
    bool rumble_present_{};
    bool rumble_active_{};
    bool large_mbc1_rom_{};
    bool mbc1_multicart_{};
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
    bool camera_registers_mapped_{};
    std::array<std::uint8_t, 0x36> camera_registers_{};
    std::vector<std::uint8_t> camera_frame_;
    std::vector<std::uint8_t> camera_image_;
    bool camera_image_dirty_{};
};

} // namespace gameboy
