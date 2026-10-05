#pragma once

#include <array>
#include <string_view>

namespace gbb {
enum class StartupMode { instant, replacement, animated };
inline constexpr std::array<StartupMode, 3> startup_modes{
    StartupMode::instant, StartupMode::replacement, StartupMode::animated};
inline constexpr std::string_view startup_description =
    "GBB replacement boot uses bundled firmware on DMG0, DMG, MGB (Pocket), CGB, SGB and SGB2. "
    "Animated boot uses scrolling monochrome or fading color GBB lettering and an APU-synthesized chime; A or Start hides the splash. "
    "SGB models run the header bootstrap without a DMG intro. Applies when the ROM is started again. No boot firmware download needed.";
[[nodiscard]] constexpr std::string_view startup_mode_id(StartupMode mode) noexcept {
    return mode == StartupMode::animated ? "animated"
         : mode == StartupMode::replacement ? "replacement" : "instant";
}
[[nodiscard]] constexpr std::string_view startup_mode_name(StartupMode mode) noexcept {
    return mode == StartupMode::animated ? "GBB animated boot"
         : mode == StartupMode::replacement ? "GBB fast boot" : "Instant startup";
}
[[nodiscard]] constexpr StartupMode startup_mode_from_setting(std::string_view value) noexcept {
    return value == "animated" ? StartupMode::animated
         : value == "replacement" ? StartupMode::replacement : StartupMode::instant;
}
} // namespace gbb
