#pragma once

#include <array>
#include <string_view>

namespace gbb {
enum class StartupMode { instant, replacement_dmg, animated_dmg };
inline constexpr std::array<StartupMode, 3> startup_modes{
    StartupMode::instant, StartupMode::replacement_dmg, StartupMode::animated_dmg};
inline constexpr std::string_view startup_description =
    "GBB replacement boot uses bundled firmware on DMG only; other models use instant startup. "
    "Animated boot adds an original GBB splash/chime; A or Start skips the splash. "
    "Applies when the ROM is started again. No firmware download needed.";
[[nodiscard]] constexpr std::string_view startup_mode_id(StartupMode mode) noexcept {
    return mode == StartupMode::animated_dmg ? "animated-dmg"
         : mode == StartupMode::replacement_dmg ? "replacement-dmg" : "instant";
}
[[nodiscard]] constexpr std::string_view startup_mode_name(StartupMode mode) noexcept {
    return mode == StartupMode::animated_dmg ? "GBB animated boot"
         : mode == StartupMode::replacement_dmg ? "GBB replacement boot" : "Instant startup";
}
[[nodiscard]] constexpr StartupMode startup_mode_from_setting(std::string_view value) noexcept {
    return value == "animated-dmg" ? StartupMode::animated_dmg
         : value == "replacement-dmg" ? StartupMode::replacement_dmg : StartupMode::instant;
}
} // namespace gbb
