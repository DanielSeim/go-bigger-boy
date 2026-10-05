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
    for (const auto mode : gbb::startup_modes) {
        check(gbb::startup_mode_from_setting(gbb::startup_mode_id(mode)) == mode,
              "hardware-neutral startup IDs round trip");
    }
    for (const auto retired : {"replacement-dmg", "animated-dmg"}) {
        check(gbb::startup_mode_from_setting(retired) == gbb::StartupMode::instant,
              "retired DMG-specific startup IDs are not accepted");
    }
    const auto directory = std::filesystem::temp_directory_path() /
                           "gbb-settings-audio-contract-test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    check(!error, "create temporary settings directory");

    AppSettings settings;
    check(settings.startup_mode == gbb::StartupMode::instant,
          "instant startup remains the default");
    settings.startup_mode = gbb::StartupMode::replacement;
    settings.audio_enabled = false;
    settings.show_fps = true;
    write_portable_settings(directory, settings);
    check(load_app_settings(directory).startup_mode == gbb::StartupMode::replacement,
          "replacement startup survives settings round trip");
    check(!load_app_settings(directory).audio_enabled,
          "audio disabled value survives settings round trip");
    check(load_app_settings(directory).show_fps,
          "FPS overlay value survives settings round trip");

    settings.audio_enabled = true;
    settings.sgb_trace_capture = true;
    write_portable_settings(directory, settings);
    check(load_app_settings(directory).audio_enabled,
          "audio enabled value survives settings round trip");
    check(load_app_settings(directory).startup_mode == gbb::StartupMode::replacement,
          "changing other settings preserves startup preference");
    check(load_app_settings(directory).sgb_trace_capture,
          "SGB trace capture value survives settings round trip");
    settings.sgb_firmware={true,std::filesystem::u8path(u8"firmware #; ünicode"),"sgb"};
    write_portable_settings(directory,settings);
    check(load_app_settings(directory).sgb_firmware==settings.sgb_firmware,
          "firmware settings and UTF-8/comment-character path round trip");
    settings.sgb_firmware.enabled=false;
    write_portable_settings(directory,settings);
    check(!load_app_settings(directory).sgb_firmware.enabled,
          "switching back to HLE preserves disabled firmware preference");

    const auto path = portable_settings_path(directory);
    {
        std::ofstream output(path, std::ios::trunc);
        output << "palette = classic\nvideo.Mode = nearest\n";
    }
    const auto migrated = load_app_settings(directory);
    check(migrated.startup_mode == gbb::StartupMode::instant,
          "legacy settings migrate to instant startup");
    check(!migrated.sgb_firmware.enabled && migrated.sgb_firmware.directory.empty() && migrated.sgb_firmware.model=="sgb2",
          "older settings retain HLE default and safe firmware defaults");
    const auto first_migration=gbb::read_settings_file(path).entries.size();
    static_cast<void>(load_app_settings(directory));
    check(gbb::read_settings_file(path).entries.size()==first_migration,
          "firmware migration is idempotent");
    check(migrated.audio_enabled,
          "settings without audio key retain the safe enabled default");
    check(!migrated.sgb_trace_capture,
          "settings without SGB trace key retain the safe disabled default");
    check(!migrated.show_fps,
          "settings without FPS key retain the safe disabled default");
    const auto document = gbb::read_settings_file(path);
    bool found_startup = false;
    for (const auto& entry : document.entries) {
        if (entry.key == "boot.Startup") {
            found_startup = true;
            check(entry.value == "instant", "migration writes instant startup default");
        }
    }
    check(found_startup, "migration writes the startup key");
    bool found_audio = false;
    for (const auto& entry : document.entries) {
        if (entry.key == "audio.Enabled") {
            found_audio = true;
            check(entry.value == "true",
                  "settings migration appends audio enabled default");
        }
    }
    check(found_audio, "settings migration writes the audio key");
    bool found_sgb_trace = false;
    for (const auto& entry : document.entries) {
        if (entry.key == "sgb.TraceCapture") {
            found_sgb_trace = true;
            check(entry.value == "false",
                  "settings migration appends SGB trace disabled default");
        }
    }
    check(found_sgb_trace, "settings migration writes the SGB trace key");
    bool found_show_fps = false;
    for (const auto& entry : document.entries) {
        if (entry.key == "video.ShowFps") {
            found_show_fps = true;
            check(entry.value == "false",
                  "settings migration appends FPS overlay disabled default");
        }
    }
    check(found_show_fps, "settings migration writes the FPS overlay key");
    {
        std::ofstream output(path, std::ios::trunc);
        output << "boot.Startup = invalid\n";
    }
    check(load_app_settings(directory).startup_mode == gbb::StartupMode::instant,
          "unknown startup values safely fall back to instant");
    for (const auto retired : {"replacement-dmg", "animated-dmg"}) {
        {
            std::ofstream output(path, std::ios::trunc);
            output << "boot.Startup = " << retired << '\n';
        }
        check(load_app_settings(directory).startup_mode == gbb::StartupMode::instant,
              "persisted retired startup values fall back without migration");
    }
    settings.startup_mode = gbb::StartupMode::animated;
    write_portable_settings(directory, settings);
    check(load_app_settings(directory).startup_mode == gbb::StartupMode::animated,
          "animated startup survives settings round trip");
    settings.startup_mode = gbb::StartupMode::instant;
    write_portable_settings(directory, settings);
    check(load_app_settings(directory).startup_mode == gbb::StartupMode::instant,
          "startup can be switched back to instant");
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
