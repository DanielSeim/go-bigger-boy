#pragma once

#ifdef _WIN32

#include "update_checker.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace gbb_desktop::artwork {

struct MetadataRecord {
    std::string name;
    std::string language;
};

[[nodiscard]] std::unordered_map<std::uint32_t, MetadataRecord> load_database(
    const std::filesystem::path& directory, const std::string& system,
    DownloadProgress* progress);
[[nodiscard]] std::string url_component(const std::string& value);
[[nodiscard]] std::string thumbnail_name(std::string name);
[[nodiscard]] std::string display_title(const std::string& canonical_name);

} // namespace gbb_desktop::artwork

#endif
