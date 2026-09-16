#pragma once

#include "gameboy/hardware_model.hpp"
#include "gameboy/rom_library.hpp"
#include "gameboy/video_pipeline.hpp"
#include "gbb/core.hpp"
#include "gbb/plugin_discovery.hpp"
#include "gbb/voxel_profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gbb_desktop {

enum class DashboardResultAction {
    resume,
    library,
    open_rom,
    quit,
    update_available,
};

using KeyboardBindings = std::array<std::array<std::int64_t, 2>, 8>;
using ActionBindings = std::array<std::int64_t, 4>;

// Value-only dashboard model. Win32 controls and worker state stay in the
// window module; this contract is shared by the dashboard and SDL runtime.
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
    gameboy::HardwareModel hardware_model{gameboy::HardwareModel::automatic};
    bool hardware_model_changed{};
    bool audio_enabled{true};
    bool audio_enabled_changed{};
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

} // namespace gbb_desktop
