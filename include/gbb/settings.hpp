#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gbb {

struct SettingsEntry {
    std::string key;
    std::string value;
};

struct SettingsDocument {
    // False means the file could not be opened. An existing but empty file is
    // readable and has an empty entry list, which is useful for migration.
    bool readable{};
    std::vector<SettingsEntry> entries;
};

// Read a simple UTF-8 key/value settings file. Blank lines and lines starting
// with '#' or ';' are ignored; inline comments use the same markers. The
// parser intentionally does not impose a schema so frontends can share the
// file format without sharing their control-specific settings types.
[[nodiscard]] SettingsDocument read_settings_file(
    const std::filesystem::path& path);

// Parse settings content without touching the filesystem. Frontends use the
// file wrapper above; tools and fuzzers can exercise identical parsing logic
// from an in-memory buffer.
[[nodiscard]] SettingsDocument parse_settings_text(std::string_view text);

} // namespace gbb
