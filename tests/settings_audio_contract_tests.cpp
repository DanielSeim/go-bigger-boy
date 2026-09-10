#include "settings_persistence.hpp"

#include "gbb/settings.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_audio_round_trip_and_migration() {
    const auto directory = std::filesystem::temp_directory_path() /
                           "gbb-settings-audio-contract-test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    check(!error, "create temporary settings directory");

    AppSettings settings;
    settings.audio_enabled = false;
    write_portable_settings(directory, settings);
    check(!load_app_settings(directory).audio_enabled,
          "audio disabled value survives settings round trip");

    settings.audio_enabled = true;
    write_portable_settings(directory, settings);
    check(load_app_settings(directory).audio_enabled,
          "audio enabled value survives settings round trip");

    const auto path = portable_settings_path(directory);
    {
        std::ofstream output(path, std::ios::trunc);
        output << "palette = classic\nvideo.Mode = nearest\n";
    }
    const auto migrated = load_app_settings(directory);
    check(migrated.audio_enabled,
          "settings without audio key retain the safe enabled default");
    const auto document = gbb::read_settings_file(path);
    bool found_audio = false;
    for (const auto& entry : document.entries) {
        if (entry.key == "audio.Enabled") {
            found_audio = true;
            check(entry.value == "true",
                  "settings migration appends audio enabled default");
        }
    }
    check(found_audio, "settings migration writes the audio key");
    std::filesystem::remove_all(directory, error);
}

void test_bool_parser() {
    check(parse_bool_setting("false", true) == false,
          "false audio setting parses as disabled");
    check(parse_bool_setting("OFF", true) == false,
          "case-insensitive off audio setting parses as disabled");
    check(parse_bool_setting("yes", false) == true,
          "yes audio setting parses as enabled");
    check(parse_bool_setting("invalid", true) == true,
          "invalid audio setting keeps the existing default");
}

} // namespace

int main() {
    test_audio_round_trip_and_migration();
    test_bool_parser();
    return failures == 0 ? 0 : 1;
}
