#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "update_checker.hpp"
#include "windows_dashboard_model.hpp"

#include "gbb/plugin_discovery.hpp"
#include "gbb/voxel_profile.hpp"

#include <windows.h>
#include <commctrl.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gbb_desktop {

struct ArtworkUpdate {
    std::size_t index{};
    std::string title;
    std::string language;
    std::filesystem::path cover;
};

// All mutable Win32 control handles and dashboard-local state have one owner.
// Behavior modules operate on this object, while the window procedure remains
// responsible for message routing and modal-loop lifetime.
struct DashboardState {
    enum class SettingsSection { general, controls, link, advanced };
    enum class Page { library, settings, shortcuts };

    const gameboy::RomLibrary* library{};
    DashboardResult result;
    bool can_resume{};
    bool voxel_available{};
    bool done{};
    HWND window{};
    HWND list{};
    HWND search_label{};
    HWND search{};
    HWND search_summary{};
    HWND search_clear{};
    HWND library_empty{};
    HWND play{};
    HWND open{};
    HWND resume{};
    HWND remove{};
    HWND palette{};
    HWND settings_heading{};
    HWND settings_status{};
    HWND settings_apply{};
    HWND settings_cancel{};
    HWND settings_section_description{};
    std::array<HWND, 4> settings_sections{};
    HWND palette_label{};
    HWND video_label{};
    HWND video{};
    HWND hardware_model{};
    HWND hardware_model_label{};
    HWND audio_enabled{};
    HWND controls_label{};
    HWND controls_instruction{};
    HWND actions_label{};
    HWND gameboy_background{};
    std::array<HWND, 8> binding_labels{};
    std::array<HWND, 2> primary_headings{};
    std::array<HWND, 2> secondary_headings{};
    std::array<std::array<HWND, 2>, 8> binding_buttons{};
    std::array<HWND, 4> action_labels{};
    std::array<HWND, 4> action_buttons{};
    HWND reset_controls{};
    HWND voxel_heading{};
    HWND voxel_fingerprint_label{};
    HWND voxel_preview{};
    std::array<HWND, 15> voxel_labels{};
    std::array<HWND, 15> voxel_edits{};
    HWND voxel_save{};
    HWND voxel_reset{};
    HWND plugin_heading{};
    HWND plugin_status{};
    HWND plugin_discovery{};
    HWND plugin_require_allowlist{};
    HWND plugin_require_capability_allowlist{};
    HWND link_heading{};
    HWND link_transport_label{};
    HWND link_transport{};
    HWND link_remote_host_label{};
    HWND link_remote_host{};
    HWND link_remote_bind_label{};
    HWND link_remote_bind{};
    HWND link_remote_port_label{};
    HWND link_remote_port{};
    HWND link_lan_discovery{};
    HWND link_bluetooth_address_label{};
    HWND link_bluetooth_address{};
    HWND link_bluetooth_choose{};
    HWND link_bluetooth_uuid_label{};
    HWND link_bluetooth_uuid{};
    HWND link_diagnostics{};
    HWND library_tab{};
    HWND settings_tab{};
    HWND shortcuts_tab{};
    HWND artwork_status{};
    HWND artwork_retry{};
    HWND shortcuts_heading{};
    HWND shortcuts_text{};
    HWND logo{};
    HBITMAP logo_bitmap{};
    HIMAGELIST covers{};
    HBRUSH background_brush{};
    HFONT ui_font{};
    Page page{Page::library};
    std::filesystem::path preference_directory;
    std::function<bool()> poll_update;
    std::filesystem::path voxel_profile_path;
    gbb::PluginDiscoveryOptions plugin_options;
    std::wstring plugin_status_text;
    std::uint64_t voxel_fingerprint{};
    gbb::VoxelProfile voxel_profile{};
    gbb::VoxelProfile initial_voxel_profile{};
    DashboardLinkSettings initial_link_settings;
    std::thread artwork_worker;
    std::atomic_bool closing{};
    DownloadProgress artwork_download;
    std::atomic_size_t artwork_completed{};
    std::atomic_size_t artwork_failed{};
    std::size_t artwork_total{};
    DashboardResult initial_result;
    bool settings_dirty{};
    SettingsSection settings_section{SettingsSection::general};
    std::wstring library_filter;
    int library_sort_column{4};
    bool library_sort_descending{true};
    int settings_scroll{};
    std::vector<HWND> settings_content_controls;
    HFONT title_font{};
    struct CapturingBinding {
        bool action{};
        std::size_t index{};
        std::size_t slot{};
        friend constexpr bool operator==(const CapturingBinding& left,
                                        const CapturingBinding& right) {
            return left.action == right.action && left.index == right.index &&
                   left.slot == right.slot;
        }
    };
    std::optional<CapturingBinding> capturing_binding;
};

} // namespace gbb_desktop

#endif
