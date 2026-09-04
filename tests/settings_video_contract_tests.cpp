#include "gameboy/display_palette.hpp"
#include "gameboy/video_pipeline.hpp"
#include "gbb/settings.hpp"
#include "gbb/video.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_settings_parser() {
    const auto path = std::filesystem::temp_directory_path() /
                      "gbb-settings-document-contract-test.ini";
    {
        std::ofstream output(path, std::ios::trunc);
        output << "# comment\n"
                  "palette = grayscale ; inline comment\n"
                  " malformed without separator\n"
                  "video.Mode= lcd\n"
                  "palette = green\n";
    }
    const auto document = gbb::read_settings_file(path);
    check(document.readable && document.entries.size() == 3 &&
              document.entries[0].key == "palette" &&
              document.entries[0].value == "grayscale" &&
              document.entries[1].key == "video.Mode" &&
              document.entries[1].value == "lcd" &&
              document.entries[2].value == "green",
              "settings parser normalizes comments and whitespace");
    const auto in_memory = gbb::parse_settings_text("alpha=1\n =bad\nbeta=2");
    check(in_memory.readable && in_memory.entries.size() == 2 &&
              in_memory.entries[0].key == "alpha" &&
              in_memory.entries[1].key == "beta",
          "in-memory settings parser shares the file parser contract");
    std::filesystem::remove(path);

    const auto empty_path = std::filesystem::temp_directory_path() /
                            "gbb-settings-document-empty-contract-test.ini";
    { std::ofstream output(empty_path, std::ios::trunc); }
    const auto empty = gbb::read_settings_file(empty_path);
    check(empty.readable && empty.entries.empty(),
          "settings parser distinguishes empty and missing files");
    std::filesystem::remove(empty_path);
    check(!gbb::read_settings_file(empty_path).readable,
          "settings parser reports missing files without throwing");

    // Exercise the parser with a reproducible matrix of whitespace, comments,
    // duplicate keys, and malformed records. The parser is intentionally
    // schema-free, so the invariant is that only non-empty keys are emitted.
    const auto mutation_path = std::filesystem::temp_directory_path() /
                               "gbb-settings-document-mutation-test.ini";
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        std::ofstream output(mutation_path, std::ios::trunc);
        output << " key" << iteration << " \t= value" << iteration
               << ((iteration & 1U) != 0 ? " ; comment" : " # comment")
               << "\n"
               << " = missing-key\n"
               << "malformed-without-separator\n"
               << "other = " << (iteration * 17U) << "\n"
               << "\t\n";
        output.close();
        const auto mutated = gbb::read_settings_file(mutation_path);
        check(mutated.readable && mutated.entries.size() == 2,
              "settings mutation matrix emits only valid entries");
        if (mutated.entries.size() == 2) {
            check(!mutated.entries[0].key.empty() &&
                      !mutated.entries[1].key.empty(),
                  "settings mutation matrix never emits an empty key");
        }
    }
    std::filesystem::remove(mutation_path);
}

void test_video_pipeline() {
    check(gameboy::video_mode_from_id("nearest") == gameboy::VideoMode::nearest &&
              gameboy::video_mode_from_id("bilinear") == gameboy::VideoMode::bilinear &&
              gameboy::video_mode_from_id("sharp") ==
                  gameboy::VideoMode::sharp_smoothing &&
              gameboy::video_mode_from_id("integer") == gameboy::VideoMode::integer &&
              gameboy::video_mode_from_id("lcd") == gameboy::VideoMode::lcd_shader &&
              gameboy::video_mode_from_id("voxel") == gameboy::VideoMode::voxel_diorama &&
              gameboy::video_mode_from_id("voxel_shape") ==
                  gameboy::VideoMode::voxel_shape &&
              gameboy::video_mode_from_id("voxel_popup") ==
                  gameboy::VideoMode::voxel_popup &&
              gameboy::video_modes.size() == 8,
          "video settings map stable ids to presentation modes");
    check(gameboy::video_mode_from_id("unknown") == gameboy::default_video_mode,
          "unknown video settings use the nearest default");
    constexpr auto source = UINT32_C(0xFFCC8844);
    check(gameboy::apply_lcd_shader(source, 1, 0) != source &&
              (gameboy::apply_lcd_shader(source, 1, 0) & UINT32_C(0xFF000000)) ==
                  UINT32_C(0xFF000000),
          "LCD shader preserves alpha while changing RGB");
    constexpr auto left = UINT32_C(0xFF202020);
    constexpr auto right = UINT32_C(0xFFE0E0E0);
    check(gameboy::apply_sharp_smoothing(source, left, right, source, source) !=
              source,
          "sharp smoothing adjusts high-contrast edges");

    const std::array<std::uint32_t, 4> frame{
        UINT32_C(0xFFFFFFFF), UINT32_C(0xFFAAAAAA),
        UINT32_C(0xFF555555), UINT32_C(0xFF000000)};
    std::vector<std::uint32_t> transformed;
    gbb::transform_video_frame(
        frame.data(), frame.size(), 2, 2, gameboy::display_palettes[1], false,
        gameboy::VideoMode::nearest, transformed);
    check(transformed.size() == frame.size() &&
              transformed[0] == gameboy::display_palettes[1].colors[0] &&
              transformed[3] == gameboy::display_palettes[1].colors[3],
          "shared video transforms apply the selected palette");
    gbb::transform_video_frame(nullptr, 0, 0, 0,
                               gameboy::display_palettes.front(), false,
                               gameboy::VideoMode::nearest, transformed);
    check(transformed.empty(),
          "shared video transforms reject invalid source frames safely");
}

} // namespace

int main() {
    test_settings_parser();
    test_video_pipeline();
    return failures == 0 ? 0 : 1;
}
