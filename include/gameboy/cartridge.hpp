#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gameboy {

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
    void flush_battery();

private:
    enum class Controller {
        rom_only,
        mbc1,
    };

    [[nodiscard]] std::uint8_t read_rom_bank(std::size_t bank,
                                             std::size_t offset) const noexcept;
    [[nodiscard]] std::size_t selected_ram_bank() const noexcept;
    void load_battery();

    std::vector<std::uint8_t> rom_;
    std::vector<std::uint8_t> ram_;
    std::filesystem::path save_path_;
    Controller controller_ = Controller::rom_only;
    std::size_t rom_bank_count_{};
    bool battery_{};
    bool large_mbc1_rom_{};
    bool ram_enabled_{};
    bool ram_dirty_{};
    std::uint8_t rom_bank_low_ = 1;
    std::uint8_t bank_upper_{};
    std::uint8_t banking_mode_{};
};

} // namespace gameboy
