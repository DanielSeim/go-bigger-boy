#ifndef GBB_DASHBOARD_NAVIGATION_HPP
#define GBB_DASHBOARD_NAVIGATION_HPP

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace gbb::desktop {

// Keep the dashboard's logical order independent from any renderer.  Native
// Windows controls, SDL's software dashboard, and headless tests can all use
// the same action contract even when their visual layouts differ.
enum class DashboardAction {
    resume,
    open_rom,
    palette,
    video,
    shortcuts,
    recent_rom,
    quit,
};

struct DashboardNavigationItem {
    DashboardAction action{};
    std::size_t recent_index{};
};

inline std::vector<DashboardNavigationItem> dashboard_navigation_items(
    const bool can_resume, const std::size_t recent_count) {
    std::vector<DashboardNavigationItem> items;
    items.reserve(recent_count + 6);
    if (can_resume) items.push_back({DashboardAction::resume, 0});
    items.push_back({DashboardAction::open_rom, 0});
    items.push_back({DashboardAction::palette, 0});
    items.push_back({DashboardAction::video, 0});
    items.push_back({DashboardAction::shortcuts, 0});
    for (std::size_t index = 0; index < recent_count; ++index) {
        items.push_back({DashboardAction::recent_rom, index});
    }
    items.push_back({DashboardAction::quit, 0});
    return items;
}

// Keyboard/gamepad navigation wraps at both ends.  A zero-item dashboard is
// handled defensively so callers can safely use this before populating data.
inline std::size_t dashboard_move_selection(const std::size_t selection,
                                             const std::size_t item_count,
                                             const int direction) {
    if (item_count == 0 || direction == 0) return 0;
    const auto current = std::min(selection, item_count - 1);
    if (direction < 0) return current == 0 ? item_count - 1 : current - 1;
    return current + 1 == item_count ? 0 : current + 1;
}

// Wheel/touch scrolling stops at the ends instead of unexpectedly jumping
// from the last item to the first.
inline std::size_t dashboard_scroll_selection(const std::size_t selection,
                                               const std::size_t item_count,
                                               const int direction) {
    if (item_count == 0 || direction == 0) return 0;
    const auto current = std::min(selection, item_count - 1);
    if (direction < 0) return current == 0 ? 0 : current - 1;
    return current + 1 == item_count ? current : current + 1;
}

inline std::size_t dashboard_first_visible(
    const std::size_t selection, const std::size_t item_count,
    const std::size_t visible_rows) {
    if (item_count == 0 || visible_rows == 0 ||
        item_count <= visible_rows || selection < visible_rows) {
        return 0;
    }
    return std::min(selection - visible_rows + 1, item_count - visible_rows);
}

} // namespace gbb::desktop

#endif
