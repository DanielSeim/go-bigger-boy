#include "windows_dashboard_artwork.hpp"

#ifdef _WIN32

#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

namespace gbb_desktop::artwork {
namespace {

std::string trimmed(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value.erase(0, first);
    const auto last = value.find_last_not_of(" \t\r\n");
    value.resize(last + 1);
    return value;
}

std::string quoted_value(const std::string& line) {
    const auto first = line.find('"');
    const auto last = line.rfind('"');
    return first != std::string::npos && last > first
               ? line.substr(first + 1, last - first - 1)
               : std::string{};
}

std::string lowercase(std::string value) {
    for (auto& character : value) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string metadata_language(const std::string& name,
                              const std::string& region) {
    const auto lower_name = lowercase(name);
    constexpr std::array<std::pair<std::string_view, std::string_view>, 14>
        tags{{{"(de", "German"}, {",de", "German"},
              {"(en", "English"}, {",en", "English"},
              {"(fr", "French"}, {",fr", "French"},
              {"(es", "Spanish"}, {",es", "Spanish"},
              {"(it", "Italian"}, {",it", "Italian"},
              {"(nl", "Dutch"}, {",nl", "Dutch"},
              {"(ja", "Japanese"}, {",ja", "Japanese"}}};
    for (const auto& [tag, language] : tags) {
        if (lower_name.find(tag) != std::string::npos) {
            return std::string{language};
        }
    }
    const auto lower_region = lowercase(region);
    constexpr std::array<std::pair<std::string_view, std::string_view>, 10>
        regions{{{"germany", "German"}, {"france", "French"},
                 {"spain", "Spanish"}, {"italy", "Italian"},
                 {"netherlands", "Dutch"}, {"japan", "Japanese"},
                 {"usa", "English"}, {"europe", "English"},
                 {"australia", "English"}, {"canada", "English"}}};
    for (const auto& [country, language] : regions) {
        if (lower_region.find(country) != std::string::npos) {
            return std::string{language};
        }
    }
    return "International";
}

std::unordered_map<std::uint32_t, MetadataRecord> parse_database(
    const std::filesystem::path& path) {
    std::unordered_map<std::uint32_t, MetadataRecord> records;
    std::ifstream input(path);
    std::string line;
    std::string name;
    std::string region;
    while (std::getline(input, line)) {
        line = trimmed(std::move(line));
        if (line == "game (") {
            name.clear();
            region.clear();
        } else if (line.rfind("name \"", 0) == 0 && name.empty()) {
            name = quoted_value(line);
        } else if (line.rfind("region \"", 0) == 0) {
            region = quoted_value(line);
        } else if (line.rfind("rom (", 0) == 0 && !name.empty()) {
            const auto marker = line.find(" crc ");
            if (marker == std::string::npos || marker + 13 > line.size()) continue;
            std::uint32_t crc{};
            std::istringstream value(line.substr(marker + 5, 8));
            if (value >> std::hex >> crc) {
                records.emplace(crc, MetadataRecord{
                    name, metadata_language(name, region)});
            }
        }
    }
    return records;
}

} // namespace

std::string url_component(const std::string& value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) || character == '-' || character == '_' ||
            character == '.' || character == '~') encoded << character;
        else encoded << '%' << std::setw(2) << std::setfill('0')
                     << static_cast<unsigned>(byte);
    }
    return encoded.str();
}

std::string thumbnail_name(std::string name) {
    constexpr std::string_view replaced = "&*/:`<>?\\|";
    for (auto& character : name) {
        if (replaced.find(character) != std::string_view::npos) character = '_';
    }
    return name;
}

std::string display_title(const std::string& canonical_name) {
    const auto tags = canonical_name.find(" (");
    return tags == std::string::npos ? canonical_name
                                     : canonical_name.substr(0, tags);
}

std::unordered_map<std::uint32_t, MetadataRecord> load_database(
    const std::filesystem::path& directory, const std::string& system,
    DownloadProgress* progress) {
    auto filename = system;
    for (auto& character : filename) {
        if (!std::isalnum(static_cast<unsigned char>(character))) character = '-';
    }
    const auto path = directory / "metadata" / (filename + ".dat");
    if (!std::filesystem::is_regular_file(path)) {
        std::string error;
        const auto url =
            "https://raw.githubusercontent.com/libretro/libretro-database/"
            "master/metadat/no-intro/" + url_component(system + ".dat");
        static_cast<void>(download_public_file(url, path, 3 * 1024 * 1024,
                                               error, progress));
    }
    return parse_database(path);
}

} // namespace gbb_desktop::artwork

#endif
