#pragma once

#include "desktop_storage.hpp"
#include "sdl_resources.hpp"

#include "gbb/dashboard_navigation.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gbb::sdl {

struct DashboardItem {
    gbb::desktop::DashboardAction action{};
    std::size_t recent_index{};
    std::string label;
};

constexpr std::size_t dashboard_visible_rows = 5;
constexpr float dashboard_first_row_y = 39.0F;
constexpr float dashboard_row_height = 18.0F;

[[nodiscard]] std::string dashboard_text(
    std::string text, std::size_t maximum = 16);
[[nodiscard]] std::vector<DashboardItem> dashboard_items(
    bool can_resume, const std::vector<std::string>& recent);
[[nodiscard]] std::size_t dashboard_first_visible(std::size_t selection,
                                                   std::size_t item_count);
[[nodiscard]] std::optional<std::size_t> dashboard_row_at(
    float logical_x, float logical_y, std::size_t selection,
    std::size_t item_count);

void activate_dashboard_selection(
    std::size_t selection, const std::vector<std::string>& recent,
    const InputBindings& bindings, gbb::EmulatorCore* core,
    DialogState& dialog, SdlResources& sdl,
    const std::filesystem::path& preference_path,
    std::optional<std::string>& pending_rom, bool& dashboard_visible,
    std::size_t& display_palette, bool& running);

} // namespace gbb::sdl
