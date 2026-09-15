#include "dashboard_controller.hpp"

#include "dialogs.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace gbb::sdl {
namespace {

[[nodiscard]] std::string rom_filename(const std::string& path) {
    auto name = std::filesystem::u8path(path).filename().u8string();
#ifdef __ANDROID__
    if (name.size() > 17 && name[16] == '-' &&
        std::all_of(name.begin(), name.begin() + 16, [](const char character) {
            return std::isxdigit(static_cast<unsigned char>(character)) != 0;
        })) {
        name.erase(0, 17);
    }
#endif
    return name.empty() ? path : name;
}

[[nodiscard]] std::string rom_display_name(const std::string& path) {
    return dashboard_text(rom_filename(path));
}

[[nodiscard]] std::string lowercase_ascii(std::string value) {
    for (auto& character : value) {
        character = static_cast<char>(std::tolower(
            static_cast<unsigned char>(character)));
    }
    return value;
}

[[nodiscard]] std::vector<std::string> filtered_recent(
    const std::vector<std::string>& recent, const std::string_view filter,
    const gameboy::RomLibrary* library) {
    if (filter.empty()) return recent;
    const auto needle = lowercase_ascii(std::string{filter});
    std::vector<std::string> result;
    result.reserve(recent.size());
    for (const auto& path : recent) {
        auto searchable = rom_filename(path);
        if (library != nullptr) {
            for (const auto& entry : library->entries()) {
                if (entry.path.u8string() == path) {
                    searchable += " " + entry.metadata.title + " " +
                                  entry.metadata.language;
                    break;
                }
            }
        }
        if (lowercase_ascii(searchable).find(needle) != std::string::npos) {
            result.push_back(path);
        }
    }
    return result;
}

} // namespace

std::string dashboard_text(std::string text, const std::size_t maximum) {
    for (auto& character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32 || byte > 126) character = '?';
    }
    if (text.size() > maximum) {
        text.resize(maximum > 3 ? maximum - 3 : maximum);
        if (maximum > 3) text += "...";
    }
    return text;
}

std::vector<DashboardItem> dashboard_items(
    const bool can_resume, const std::vector<std::string>& recent,
    const std::string_view filter, const gameboy::RomLibrary* library) {
    const auto visible_recent = filtered_recent(recent, filter, library);
    std::vector<DashboardItem> items;
    const auto navigation = gbb::desktop::dashboard_navigation_items(
        can_resume, visible_recent.size());
    items.reserve(navigation.size() + 1);
    for (const auto& item : navigation) {
        if (item.action == gbb::desktop::DashboardAction::quit &&
            !filter.empty() && visible_recent.empty()) {
            items.push_back({gbb::desktop::DashboardAction::no_matching_games,
                             0, "No games match"});
        }
        std::string label;
        switch (item.action) {
        case gbb::desktop::DashboardAction::resume:
            label = "Resume game";
            break;
        case gbb::desktop::DashboardAction::open_rom:
            label = "+ Open a ROM";
            break;
        case gbb::desktop::DashboardAction::palette:
            label = "Display palette";
            break;
        case gbb::desktop::DashboardAction::video:
            label = "Video pipeline";
            break;
        case gbb::desktop::DashboardAction::shortcuts:
            label = "Keyboard shortcuts";
            break;
        case gbb::desktop::DashboardAction::recent_rom:
            label = rom_display_name(visible_recent[item.recent_index]);
            if (library != nullptr) {
                for (const auto& entry : library->entries()) {
                    if (entry.path.u8string() == visible_recent[item.recent_index] &&
                        !entry.metadata.title.empty()) {
                        label = dashboard_text(entry.metadata.title);
                        break;
                    }
                }
            }
            break;
        case gbb::desktop::DashboardAction::no_matching_games:
            label = "No games match";
            break;
        case gbb::desktop::DashboardAction::quit:
            label = "Exit GBB";
            break;
        }
        auto path = item.action == gbb::desktop::DashboardAction::recent_rom
                        ? visible_recent[item.recent_index]
                        : std::string{};
        items.push_back({item.action, item.recent_index, std::move(label),
                         std::move(path)});
    }
    return items;
}

std::size_t dashboard_first_visible(const std::size_t selection,
                                    const std::size_t item_count) {
    return gbb::desktop::dashboard_first_visible(
        selection, item_count, dashboard_visible_rows);
}

std::optional<std::size_t> dashboard_row_at(
    const float logical_x, const float logical_y, const std::size_t selection,
    const std::size_t item_count) {
    if (logical_x < 9.0F || logical_x > 151.0F ||
        logical_y < dashboard_first_row_y) {
        return std::nullopt;
    }
    const auto row = static_cast<std::size_t>(
        (logical_y - dashboard_first_row_y) / dashboard_row_height);
    if (row >= dashboard_visible_rows) return std::nullopt;
    const auto index = dashboard_first_visible(selection, item_count) + row;
    return index < item_count ? std::optional<std::size_t>{index}
                              : std::nullopt;
}

void activate_dashboard_selection(
    const std::size_t selection, const std::vector<std::string>& recent,
    const InputBindings& bindings, gbb::EmulatorCore* core,
    DialogState& dialog, SdlResources& sdl,
    const std::filesystem::path& preference_path,
    std::optional<std::string>& pending_rom, bool& dashboard_visible,
    std::size_t& display_palette, bool& running,
    const std::string_view filter, const gameboy::RomLibrary* library) {
    const auto items = dashboard_items(core != nullptr, recent, filter, library);
    const auto visible_recent = filtered_recent(recent, filter, library);
    if (selection >= items.size()) return;
    const auto& item = items[selection];
    switch (item.action) {
    case gbb::desktop::DashboardAction::resume:
        dashboard_visible = false;
        break;
    case gbb::desktop::DashboardAction::open_rom:
        show_rom_dialog(dialog, sdl.window);
        break;
    case gbb::desktop::DashboardAction::palette:
        choose_display_palette(core, sdl, preference_path, display_palette);
        break;
    case gbb::desktop::DashboardAction::video:
        choose_video_mode(sdl, preference_path);
        break;
    case gbb::desktop::DashboardAction::shortcuts:
        show_help(sdl.window, bindings);
        break;
    case gbb::desktop::DashboardAction::recent_rom:
        if (item.recent_index < visible_recent.size()) {
            pending_rom = visible_recent[item.recent_index];
        }
        break;
    case gbb::desktop::DashboardAction::no_matching_games:
        // The row explains the empty state; Escape clears the filter.
        break;
    case gbb::desktop::DashboardAction::quit:
        if (confirm_exit(sdl.window)) running = false;
        break;
    }
}

} // namespace gbb::sdl
