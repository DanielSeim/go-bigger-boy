#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gameboy {

enum class RomPlatform { game_boy, game_boy_color };

struct RomMetadata {
    std::uint64_t fingerprint{};
    std::uint32_t crc32{};
    std::string title;
    RomPlatform platform{RomPlatform::game_boy};
    std::string language;
    std::string cover_name;
};

struct RomLibraryEntry {
    std::filesystem::path path;
    RomMetadata metadata;
    std::int64_t last_played{};
};

[[nodiscard]] RomMetadata inspect_rom(
    const std::vector<std::uint8_t>& bytes,
    const std::filesystem::path& source_name = {});
[[nodiscard]] RomMetadata inspect_rom_file(const std::filesystem::path& path);
[[nodiscard]] const char* platform_name(RomPlatform platform) noexcept;
[[nodiscard]] const char* cover_system_name(RomPlatform platform) noexcept;

class RomLibrary {
public:
    static constexpr std::size_t maximum_entries = 12;

    static RomLibrary load(const std::filesystem::path& preference_directory);
    void remember(const std::filesystem::path& path, RomMetadata metadata,
                  std::int64_t last_played = 0);
    bool remove(std::uint64_t fingerprint);
    void save(const std::filesystem::path& preference_directory) const;

    [[nodiscard]] const std::vector<RomLibraryEntry>& entries() const noexcept;

private:
    std::vector<RomLibraryEntry> entries_;
};

} // namespace gameboy
