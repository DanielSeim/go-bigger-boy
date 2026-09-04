#include "gbb/settings.hpp"

#include <fstream>
#include <iterator>
#include <utility>

namespace gbb {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value.erase(0, first);
    const auto last = value.find_last_not_of(" \t\r\n");
    value.resize(last + 1);
    return value;
}

} // namespace

SettingsDocument parse_settings_text(const std::string_view text) {
    SettingsDocument document;
    document.readable = true;

    std::size_t offset{};
    std::string line;
    while (offset <= text.size()) {
        const auto end = text.find('\n', offset);
        line.assign(text.substr(offset, end == std::string_view::npos
                                      ? text.size() - offset
                                      : end - offset));
        const auto comment = line.find_first_of("#;");
        if (comment != std::string::npos) line.resize(comment);
        const auto separator = line.find('=');
        if (separator != std::string::npos) {
            auto key = trim(line.substr(0, separator));
            auto value = trim(line.substr(separator + 1));
            if (!key.empty())
                document.entries.push_back({std::move(key), std::move(value)});
        }
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return document;
}

SettingsDocument read_settings_file(const std::filesystem::path& path) {
    SettingsDocument document;
    if (path.empty()) return document;
    std::ifstream input(path);
    if (!input) return document;
    document.readable = true;
    return parse_settings_text(
        std::string{std::istreambuf_iterator<char>{input},
                    std::istreambuf_iterator<char>{}});
}

} // namespace gbb
