#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>

namespace gbb::sdl {

// The debugger is intentionally laid out from one geometry description.  The
// renderer and hit testing must use the same rectangles or a resize can make
// a visible control disagree with its clickable area.
struct DesktopDebuggerLayout {
    SDL_FRect map{};
    SDL_FRect register_area{};
    SDL_FRect disassembly{};
    SDL_FRect inspector{};
    SDL_FRect memory{};
    SDL_FRect video_viewers{};
    SDL_FRect hardware_inspector{};
    SDL_FRect breakpoint_toggle{};
    SDL_FRect breakpoint_clear{};
    SDL_FRect record{};
    SDL_FRect replay{};
    SDL_FRect tas{};
    SDL_FRect run_pause{};
    SDL_FRect step_cpu{};
    SDL_FRect step_frame{};
    SDL_FRect sprite_editor{};
    float map_scale{2.0F};
};

inline constexpr int desktop_debugger_minimum_width = 960;
inline constexpr int desktop_debugger_minimum_height = 700;

[[nodiscard]] inline bool desktop_debugger_rect_contains(
    const SDL_FRect& rect, const float x, const float y) noexcept {
    return x >= rect.x && x <= rect.x + rect.w && y >= rect.y &&
           y <= rect.y + rect.h;
}

[[nodiscard]] inline DesktopDebuggerLayout desktop_debugger_layout(
    const int requested_width, const int requested_height) noexcept {
    const auto width = static_cast<float>(std::max(
        requested_width, desktop_debugger_minimum_width));
    const auto height = static_cast<float>(std::max(
        requested_height, desktop_debugger_minimum_height));
    constexpr float margin = 24.0F;
    constexpr float map_y = 72.0F;
    constexpr float action_top = 174.0F;
    constexpr float column_gap = 16.0F;

    const auto disassembly_width = std::clamp(width * 0.32F, 300.0F, 440.0F);
    const auto disassembly_x = width - margin - disassembly_width;
    const auto left_width = disassembly_x - margin * 2.0F - column_gap;
    const auto map_width_by_height = height - 254.0F;
    const auto map_width = std::clamp(
        std::min(512.0F, std::min(left_width * 0.67F, map_width_by_height)),
        320.0F, 512.0F);
    const auto register_x = margin + map_width + column_gap;

    DesktopDebuggerLayout layout;
    layout.map = {margin, map_y, map_width, map_width};
    layout.map_scale = map_width / 256.0F;
    layout.register_area = {register_x, map_y,
                            std::max(160.0F, disassembly_x - register_x -
                                                   column_gap),
                            map_width};
    layout.disassembly = {disassembly_x, 64.0F, disassembly_width,
                          std::max(120.0F, height - action_top - 64.0F)};
    layout.inspector = {margin, 64.0F,
                        std::max(320.0F, disassembly_x - margin - column_gap),
                        layout.disassembly.h};
    layout.memory = {layout.inspector.x + 8.0F,
                     layout.inspector.y + 302.0F,
                     layout.inspector.w - 16.0F,
                     std::max(36.0F, layout.inspector.h - 310.0F)};

    // Header navigation is deliberately real UI, not shortcut-only text.
    layout.video_viewers = {250.0F, 12.0F, 142.0F, 36.0F};
    layout.hardware_inspector = {400.0F, 12.0F, 190.0F, 36.0F};

    const auto breakpoint_y = height - 154.0F;
    const auto movie_y = height - 106.0F;
    const auto button_y = height - 58.0F;
    layout.breakpoint_toggle = {margin, breakpoint_y, 250.0F, 36.0F};
    layout.breakpoint_clear = {288.0F, breakpoint_y, 200.0F, 36.0F};
    layout.record = {margin, movie_y, 170.0F, 36.0F};
    layout.replay = {208.0F, movie_y, 170.0F, 36.0F};
    layout.tas = {734.0F, movie_y, 150.0F, 36.0F};
    layout.run_pause = {margin, button_y, 150.0F, 36.0F};
    layout.step_cpu = {188.0F, button_y, 170.0F, 36.0F};
    layout.step_frame = {372.0F, button_y, 150.0F, 36.0F};
    layout.sprite_editor = {536.0F, button_y, 150.0F, 36.0F};
    return layout;
}

[[nodiscard]] inline std::size_t desktop_debugger_line_count(
    const SDL_FRect& panel) noexcept {
    constexpr std::size_t line_height = 24;
    const auto available = panel.h > 36.0F ? panel.h - 36.0F : 0.0F;
    return std::clamp(static_cast<std::size_t>(available / line_height),
                      std::size_t{4}, std::size_t{20});
}

[[nodiscard]] inline std::size_t desktop_debugger_memory_row_count(
    const SDL_FRect& panel) noexcept {
    const auto available = panel.h > 25.0F ? panel.h - 25.0F : 0.0F;
    return std::clamp(static_cast<std::size_t>(available / 14.0F),
                      std::size_t{1}, std::size_t{16});
}

} // namespace gbb::sdl
