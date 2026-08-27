#include "gbb/voxel_profile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gbb {
namespace {

constexpr std::string_view super_mario_land_profile =
    "[0x7eafc0023b31d850]\n"
    "; SUPER MARIOLAND (W) (V1.0) [!] — layered platformer scene\n"
    "depth_scale=1.25\n"
    "camera_pitch=24\n"
    "camera_yaw=0\n"
    "zoom=0.74\n"
    "perspective=0.0012\n"
    "sprite_depth=10\n"
    "lighting=1.08\n"
    "background_depth_far=100\n"
    "background_depth_near=20\n"
    "background_transparent_depth=95\n"
    "window_depth_far=90\n"
    "window_depth_near=50\n"
    "sprite_depth_far=45\n"
    "sprite_depth_near=25\n"
    "framebuffer_facade=0\n";

std::string trim(std::string value) {
    const auto not_space = [](const unsigned char c) {
        return !std::isspace(c);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

bool parse_float(const std::string& text, float& target) {
    try {
        std::size_t consumed = 0;
        const auto value = std::stof(text, &consumed);
        if (consumed != text.size() || !std::isfinite(value)) return false;
        target = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_bool(const std::string& text, bool& target) {
    if (text == "1" || text == "true" || text == "yes") {
        target = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "no") {
        target = false;
        return true;
    }
    return false;
}

void parse_key(VoxelProfile& profile, const std::string& key,
               const std::string& value) {
    if (key == "depth_scale") parse_float(value, profile.depth_scale);
    else if (key == "camera_pitch") parse_float(value, profile.camera_pitch);
    else if (key == "camera_yaw") parse_float(value, profile.camera_yaw);
    else if (key == "zoom") parse_float(value, profile.zoom);
    else if (key == "perspective") parse_float(value, profile.perspective);
    else if (key == "sprite_depth") parse_float(value, profile.sprite_depth);
    else if (key == "lighting") parse_float(value, profile.lighting);
    else if (key == "background_depth_far") parse_float(value, profile.background_depth_far);
    else if (key == "background_depth_near") parse_float(value, profile.background_depth_near);
    else if (key == "background_transparent_depth") parse_float(value, profile.background_transparent_depth);
    else if (key == "window_depth_far") parse_float(value, profile.window_depth_far);
    else if (key == "window_depth_near") parse_float(value, profile.window_depth_near);
    else if (key == "sprite_depth_far") parse_float(value, profile.sprite_depth_far);
    else if (key == "sprite_depth_near") parse_float(value, profile.sprite_depth_near);
    else if (key == "framebuffer_facade") parse_bool(value, profile.framebuffer_facade);
}

void clamp_profile(VoxelProfile& profile) {
    profile.depth_scale = std::clamp(profile.depth_scale, 0.0F, 8.0F);
    profile.camera_pitch = std::clamp(profile.camera_pitch, -80.0F, 80.0F);
    profile.camera_yaw = std::clamp(profile.camera_yaw, -180.0F, 180.0F);
    profile.zoom = std::clamp(profile.zoom, 0.25F, 4.0F);
    profile.perspective = std::clamp(profile.perspective, 0.0F, 0.02F);
    profile.sprite_depth = std::clamp(profile.sprite_depth, 0.0F, 64.0F);
    profile.lighting = std::clamp(profile.lighting, 0.1F, 2.0F);
    profile.background_depth_far = std::clamp(profile.background_depth_far, 1.0F, 1000.0F);
    profile.background_depth_near = std::clamp(profile.background_depth_near, 0.0F, profile.background_depth_far - 0.01F);
    profile.background_transparent_depth = std::clamp(profile.background_transparent_depth,
                                                       profile.background_depth_near,
                                                       profile.background_depth_far);
    profile.window_depth_far = std::clamp(profile.window_depth_far, 0.0F, 1000.0F);
    profile.window_depth_near = std::clamp(profile.window_depth_near, 0.0F, profile.window_depth_far - 0.01F);
    profile.sprite_depth_far = std::clamp(profile.sprite_depth_far, 0.0F, 1000.0F);
    profile.sprite_depth_near = std::clamp(profile.sprite_depth_near, 0.0F, profile.sprite_depth_far - 0.01F);
}

bool section_matches(const std::string& section, const std::uint64_t fingerprint) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(section, &consumed, 0);
        if (consumed == section.size() && parsed == fingerprint) return true;
    } catch (...) {
    }
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(section, &consumed, 16);
        return consumed == section.size() && parsed == fingerprint;
    } catch (...) {
        return false;
    }
}

} // namespace

VoxelProfile built_in_voxel_profile(const std::uint64_t fingerprint) {
    VoxelProfile profile;
    if (fingerprint == UINT64_C(0x7eafc0023b31d850)) {
        profile.depth_scale = 1.25F;
        profile.camera_pitch = 24.0F;
        profile.camera_yaw = 0.0F;
        profile.zoom = 0.74F;
        profile.perspective = 0.0012F;
    profile.sprite_depth = 10.0F;
    profile.lighting = 1.08F;
    profile.background_depth_far = 100.0F;
    profile.background_depth_near = 20.0F;
    profile.background_transparent_depth = 95.0F;
    profile.window_depth_far = 90.0F;
    profile.window_depth_near = 50.0F;
    profile.sprite_depth_far = 45.0F;
    profile.sprite_depth_near = 25.0F;
    profile.framebuffer_facade = false;
    }
    return profile;
}

VoxelProfile load_voxel_profile(const std::filesystem::path& path,
                                const std::uint64_t fingerprint) {
    VoxelProfile defaults = built_in_voxel_profile(fingerprint);
    VoxelProfile selected = defaults;
    std::ifstream input(path);
    if (!input) return selected;
    std::string section = "default";
    std::optional<VoxelProfile> rom_profile;
    bool active_rom_section = false;
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            active_rom_section = section != "default" &&
                                 section_matches(section, fingerprint);
            if (active_rom_section) rom_profile = defaults;
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        auto* profile = section == "default" ? &defaults :
            (active_rom_section && rom_profile ? &*rom_profile : nullptr);
        if (profile == nullptr) continue;
        parse_key(*profile, trim(line.substr(0, separator)),
                  trim(line.substr(separator + 1)));
    }
    selected = rom_profile ? *rom_profile : defaults;
    clamp_profile(selected);
    return selected;
}

bool save_voxel_profile(const std::filesystem::path& path,
                        const std::uint64_t fingerprint,
                        const VoxelProfile& profile) {
    if (path.empty()) return false;
    VoxelProfile clamped = profile;
    clamp_profile(clamped);
    std::vector<std::string> lines;
    {
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) lines.push_back(std::move(line));
    }
    std::ostringstream section;
    section << "[0x" << std::hex << fingerprint << std::dec << "]\n"
            << "depth_scale=" << clamped.depth_scale << '\n'
            << "camera_pitch=" << clamped.camera_pitch << '\n'
            << "camera_yaw=" << clamped.camera_yaw << '\n'
            << "zoom=" << clamped.zoom << '\n'
            << "perspective=" << clamped.perspective << '\n'
            << "sprite_depth=" << clamped.sprite_depth << '\n'
            << "lighting=" << clamped.lighting << '\n'
            << "background_depth_far=" << clamped.background_depth_far << '\n'
            << "background_depth_near=" << clamped.background_depth_near << '\n'
            << "background_transparent_depth=" << clamped.background_transparent_depth << '\n'
            << "window_depth_far=" << clamped.window_depth_far << '\n'
            << "window_depth_near=" << clamped.window_depth_near << '\n'
            << "sprite_depth_far=" << clamped.sprite_depth_far << '\n'
            << "sprite_depth_near=" << clamped.sprite_depth_near << '\n'
            << "framebuffer_facade=" << (clamped.framebuffer_facade ? 1 : 0);
    std::vector<std::string> replacement;
    std::istringstream section_input(section.str());
    std::string replacement_line;
    while (std::getline(section_input, replacement_line)) {
        replacement.push_back(std::move(replacement_line));
    }
    std::size_t section_start = lines.size();
    std::size_t section_end = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto line = trim(lines[index]);
        if (line.size() < 2 || line.front() != '[' || line.back() != ']') continue;
        if (section_matches(trim(line.substr(1, line.size() - 2)), fingerprint)) {
            section_start = index;
            section_end = lines.size();
            for (std::size_t next = index + 1; next < lines.size(); ++next) {
                const auto next_line = trim(lines[next]);
                if (next_line.size() >= 2 && next_line.front() == '[' &&
                    next_line.back() == ']') {
                    section_end = next;
                    break;
                }
            }
            break;
        }
    }
    if (section_start == lines.size()) {
        if (!lines.empty() && !lines.back().empty()) lines.push_back({});
        lines.insert(lines.end(), replacement.begin(), replacement.end());
    } else {
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(section_start),
                    lines.begin() + static_cast<std::ptrdiff_t>(section_end));
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(section_start),
                     replacement.begin(), replacement.end());
    }
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) return false;
    for (const auto& line : lines) output << line << '\n';
    output.close();
    if (!output) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return false;
    }
    std::error_code error;
    const auto backup = path.string() + ".bak";
    std::filesystem::remove(backup, error);
    error.clear();
    const auto had_existing = std::filesystem::exists(path, error);
    error.clear();
    if (had_existing) {
        std::filesystem::rename(path, backup, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        error.clear();
        if (had_existing) std::filesystem::rename(backup, path, error);
        return false;
    }
    std::filesystem::remove(backup, error);
    return true;
}

void ensure_voxel_profile_file(const std::filesystem::path& path) {
    if (path.empty()) return;
    if (std::filesystem::exists(path)) {
        // Existing installations receive new built-in profiles once, while
        // preserving any values the user has already configured.
        std::ifstream input(path);
        const std::string contents((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
        if (contents.find("[0x7eafc0023b31d850]") != std::string::npos) return;
        std::ofstream output(path, std::ios::app);
        if (!output) return;
        output << '\n' << super_mario_land_profile;
        return;
    }
    std::ofstream output(path);
    if (!output) return;
    output << "; Go Bigger Boy voxel diorama profiles\n"
              "; Add a [0xROM_FINGERPRINT] section to override [default].\n"
              "[default]\n"
              "depth_scale=1.0\n"
              "camera_pitch=24\n"
              "camera_yaw=0\n"
              "zoom=0.72\n"
              "perspective=0.0015\n"
              "sprite_depth=8\n"
              "lighting=1.0\n"
              "background_depth_far=100\n"
              "background_depth_near=20\n"
              "background_transparent_depth=95\n"
              "window_depth_far=90\n"
              "window_depth_near=50\n"
              "sprite_depth_far=45\n"
              "sprite_depth_near=25\n"
              "framebuffer_facade=0\n\n"
           << super_mario_land_profile;
}

} // namespace gbb
