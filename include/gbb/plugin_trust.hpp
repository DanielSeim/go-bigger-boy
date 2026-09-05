#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace gbb {

// Computes a lowercase hexadecimal SHA-256 digest for a plugin file. The file
// is read as bytes and never executed. A null result reports an I/O error.
[[nodiscard]] std::optional<std::string> plugin_sha256_file(
    const std::filesystem::path& path, std::string& error);

} // namespace gbb
