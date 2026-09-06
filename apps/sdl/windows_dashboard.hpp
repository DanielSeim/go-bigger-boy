#pragma once

#include "gameboy/rom_library.hpp"
#include "gameboy/video_pipeline.hpp"
#include "gbb/voxel_profile.hpp"
#include "gbb/core.hpp"
#include "gbb/plugin_discovery.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

namespace gbb_desktop {

enum class DashboardResultAction { resume, open_rom, quit, update_available };

using KeyboardBindings = std::array<std::array<std::int64_t, 2>, 8>;
using ActionBindings = std::array<std::int64_t, 4>;

// Settings shared by the desktop dashboard and the remote-link controller.
// Keeping this as one value object avoids a growing list of loosely related
// parameters as additional transports are added.
struct DashboardLinkSettings {
    std::string transport{"tcp"};
    std::string remote_host{"127.0.0.1"};
    std::string remote_bind{"127.0.0.1"};
    std::uint16_t remote_port{8765};
    bool lan_discovery{};
    std::string bluetooth_address;
    std::string bluetooth_service_uuid{
        "7b8f5d6e-7a47-4e17-9f9d-4b4d9d8e4f3a"};
    bool diagnostics{};
};

struct DashboardResult {
    DashboardResultAction action{DashboardResultAction::resume};
    std::string rom_path;
    std::size_t palette{};
    bool palette_changed{};
    gameboy::VideoMode video_mode{gameboy::default_video_mode};
    bool video_mode_changed{};
    KeyboardBindings keyboard_bindings{};
    bool keyboard_bindings_changed{};
    ActionBindings action_bindings{};
    bool action_bindings_changed{};
    bool voxel_profile_changed{};
    bool plugin_discovery{};
    bool plugin_require_allowlist{};
    bool plugin_require_capability_allowlist{};
    bool plugin_settings_changed{};
    DashboardLinkSettings link_settings;
    bool link_settings_changed{};
    std::vector<std::uint64_t> removed_fingerprints;
};

DashboardResult show_windows_dashboard(
    HWND owner, const gameboy::RomLibrary& library, bool can_resume,
    std::uint64_t current_fingerprint,
    gbb::CoreCapability capabilities,
    std::size_t palette, gameboy::VideoMode video_mode,
    const KeyboardBindings& keyboard_bindings,
    const ActionBindings& action_bindings,
    const DashboardLinkSettings& link_settings,
    const gbb::PluginDiscoveryOptions& plugin_options,
    const gbb::PluginCatalog& plugin_catalog,
    const std::filesystem::path& preference_directory,
    // Called on the dashboard's UI thread while its modal loop is running.
    // Returning true closes the dashboard so the caller can present the
    // update offer without competing with native controls.
    const std::function<bool()>& poll_update);

} // namespace gbb_desktop
#endif
