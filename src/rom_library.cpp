#include "gameboy/rom_library.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace gameboy {
namespace {

constexpr std::size_t minimum_header_size = 0x150;

std::string source_stem(const std::filesystem::path& source_name) {
    auto stem = source_name.stem().u8string();
    if (stem.size() > 17 && stem[16] == '-' &&
        std::all_of(stem.begin(), stem.begin() + 16,
                    [](const unsigned char character) {
                        return std::isxdigit(character) != 0;
                    })) {
        stem.erase(0, 17);
    }
    return stem;
}

std::string trimmed_ascii_title(const std::vector<std::uint8_t>& bytes) {
    const auto cgb = (bytes[0x143] & 0x80) != 0;
    const auto end = cgb ? std::size_t{0x143} : std::size_t{0x144};
    std::string title;
    for (auto index = std::size_t{0x134}; index < end; ++index) {
        const auto byte = bytes[index];
        if (byte == 0) break;
        title.push_back(byte >= 32 && byte <= 126
                            ? static_cast<char>(byte)
                            : '?');
    }
    while (!title.empty() && title.back() == ' ') title.pop_back();
    return title;
}

std::string filename_title(const std::filesystem::path& source_name) {
    auto title = source_stem(source_name);
    const auto tag = title.find(" (");
    if (tag != std::string::npos) title.resize(tag);
    return title;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

void append_language(std::string& result, const std::string_view language) {
    if (!result.empty()) result += ", ";
    result += language;
}

std::string language_from_filename(const std::filesystem::path& source_name,
                                   const bool japanese_destination) {
    const auto filename = lowercase(source_stem(source_name));
    std::string languages;
    constexpr std::pair<std::string_view, std::string_view> codes[]{
        {"en", "English"}, {"ja", "Japanese"}, {"fr", "French"},
        {"de", "German"},  {"es", "Spanish"},  {"it", "Italian"},
        {"nl", "Dutch"},   {"pt", "Portuguese"}, {"sv", "Swedish"},
        {"no", "Norwegian"}, {"da", "Danish"}, {"fi", "Finnish"},
    };
    for (const auto& [code, name] : codes) {
        const auto tagged = std::string{"("} + std::string{code};
        const auto listed = std::string{","} + std::string{code};
        if (filename.find(tagged) != std::string::npos ||
            filename.find(listed) != std::string::npos) {
            append_language(languages, name);
        }
    }
    if (!languages.empty()) return languages;
    if (filename.find("(japan") != std::string::npos) return "Japanese";
    if (filename.find("(usa") != std::string::npos) return "English";
    if (filename.find("(world") != std::string::npos) return "International";
    return japanese_destination ? "Japanese" : "International";
}

std::string cover_name(const std::filesystem::path& source_name,
                       const std::string& fallback) {
    auto name = source_stem(source_name);
    if (name.empty()) name = fallback;
    constexpr std::string_view replaced = "&*/:`<>?\\|";
    for (auto& character : name) {
        if (replaced.find(character) != std::string_view::npos) character = '_';
    }
    return name;
}

std::int64_t current_time() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

RomMetadata inspect_rom(const std::vector<std::uint8_t>& bytes,
                        const std::filesystem::path& source_name) {
    if (bytes.size() < minimum_header_size) {
        throw std::invalid_argument("ROM is too small to contain a Game Boy header");
    }
    RomMetadata metadata;
    metadata.fingerprint = UINT64_C(14695981039346656037);
    for (const auto byte : bytes) {
        metadata.fingerprint ^= byte;
        metadata.fingerprint *= UINT64_C(1099511628211);
    }
    metadata.title = trimmed_ascii_title(bytes);
    if (metadata.title.empty()) metadata.title = filename_title(source_name);
    if (metadata.title.empty()) metadata.title = "Unknown game";
    metadata.platform = (bytes[0x143] & 0x80) != 0
                            ? RomPlatform::game_boy_color
                            : RomPlatform::game_boy;
    metadata.language = language_from_filename(source_name, bytes[0x14A] == 0);
    metadata.cover_name = cover_name(source_name, metadata.title);
    return metadata;
}

RomMetadata inspect_rom_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open ROM metadata source");
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input}, {}};
    return inspect_rom(bytes, path.filename());
}

const char* platform_name(const RomPlatform platform) noexcept {
    return platform == RomPlatform::game_boy_color ? "Game Boy Color"
                                                    : "Game Boy";
}

const char* cover_system_name(const RomPlatform platform) noexcept {
    return platform == RomPlatform::game_boy_color
               ? "Nintendo - Game Boy Color"
               : "Nintendo - Game Boy";
}

RomLibrary RomLibrary::load(const std::filesystem::path& preference_directory) {
    RomLibrary library;
    std::ifstream input(preference_directory / "rom-library.txt");
    std::uint64_t fingerprint = 0;
    std::int64_t last_played = 0;
    int platform = 0;
    std::string path;
    std::string title;
    std::string language;
    std::string cover;
    while (library.entries_.size() < maximum_entries &&
           input >> std::hex >> fingerprint >> std::dec >> last_played >>
               platform >> std::quoted(path) >> std::quoted(title) >>
               std::quoted(language) >> std::quoted(cover)) {
        if (path.empty() || fingerprint == 0 || platform < 0 || platform > 1) {
            continue;
        }
        library.remember(
            std::filesystem::u8path(path),
            {fingerprint, std::move(title), static_cast<RomPlatform>(platform),
             std::move(language), std::move(cover)},
            last_played);
    }
    std::stable_sort(library.entries_.begin(), library.entries_.end(),
                     [](const RomLibraryEntry& left,
                        const RomLibraryEntry& right) {
                         return left.last_played > right.last_played;
                     });
    return library;
}

void RomLibrary::remember(const std::filesystem::path& path,
                          RomMetadata metadata, std::int64_t last_played) {
    const auto normalized = path.lexically_normal();
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [&](const RomLibraryEntry& entry) {
                           return entry.metadata.fingerprint ==
                                      metadata.fingerprint ||
                                  entry.path.lexically_normal() == normalized;
                       }),
        entries_.end());
    entries_.insert(entries_.begin(),
                    {normalized, std::move(metadata),
                     last_played == 0 ? current_time() : last_played});
    if (entries_.size() > maximum_entries) entries_.resize(maximum_entries);
}

void RomLibrary::save(
    const std::filesystem::path& preference_directory) const {
    if (preference_directory.empty()) return;
    std::filesystem::create_directories(preference_directory);
    std::ofstream output(preference_directory / "rom-library.txt",
                         std::ios::trunc);
    for (const auto& entry : entries_) {
        output << std::hex << entry.metadata.fingerprint << std::dec << ' '
               << entry.last_played << ' '
               << static_cast<int>(entry.metadata.platform) << ' '
               << std::quoted(entry.path.u8string()) << ' '
               << std::quoted(entry.metadata.title) << ' '
               << std::quoted(entry.metadata.language) << ' '
               << std::quoted(entry.metadata.cover_name) << '\n';
    }
}

const std::vector<RomLibraryEntry>& RomLibrary::entries() const noexcept {
    return entries_;
}

} // namespace gameboy
