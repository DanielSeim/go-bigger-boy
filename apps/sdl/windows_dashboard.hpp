#pragma once

#include "windows_dashboard_model.hpp"

#include <functional>

#ifdef _WIN32
#include <windows.h>

namespace gbb_desktop {

DashboardResult show_windows_dashboard(
    HWND owner, const gameboy::RomLibrary& library, bool can_resume,
    std::uint64_t current_fingerprint,
    gbb::CoreCapability capabilities,
    std::size_t palette, gameboy::VideoMode video_mode,
    gameboy::HardwareModel hardware_model,
    bool audio_enabled,
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
