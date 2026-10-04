#pragma once
#include "gameboy/video_pipeline.hpp"
#include "gbb/core_registry.hpp"
#include "gbb/sgb_firmware_core.hpp"
#include <filesystem>
#include <string>
#include <stdexcept>

namespace gbb::sdl {
struct DesktopFirmwareSettings {
    bool enabled{};
    std::filesystem::path directory;
    std::string model{"sgb2"};
    friend bool operator==(const DesktopFirmwareSettings& a, const DesktopFirmwareSettings& b) {
        return a.enabled==b.enabled && a.directory==b.directory && a.model==b.model;
    }
};
inline bool firmware_video_supported(gameboy::VideoMode mode) noexcept {
    return mode!=gameboy::VideoMode::voxel_diorama && mode!=gameboy::VideoMode::voxel_shape && mode!=gameboy::VideoMode::voxel_popup;
}
inline void validate_desktop_firmware(const DesktopFirmwareSettings& settings) {
    if(!settings.enabled) return;
    if(settings.directory.empty()) throw std::invalid_argument("Select a caller-owned SGB firmware directory before enabling firmware playback.");
    gbb::validate_sgb_firmware_images(settings.directory,settings.model);
}
// Encode UTF-8 paths to preserve INI comment characters and whitespace without
// changing the common settings parser. Plain paths are also accepted on read.
inline std::string firmware_directory_setting(const std::filesystem::path& path) {
    constexpr char hex[]="0123456789abcdef";
    std::string result="hex:";
    for(unsigned char byte:path.u8string()) { result+=hex[byte>>4]; result+=hex[byte&15]; }
    return result;
}
inline std::filesystem::path firmware_directory_from_setting(const std::string& value) {
    if(value.substr(0,4)!="hex:") return std::filesystem::u8path(value);
    const auto digit=[](char c)->int { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; };
    std::string decoded;
    if((value.size()-4)%2) return std::filesystem::u8path(value); // Fails explicitly at validation/launch.
    for(std::size_t n=4;n<value.size();n+=2) {
        const auto a=digit(value[n]), b=digit(value[n+1]);
        if(a<0||b<0||a*16+b==0) return std::filesystem::u8path(value);
        decoded+=static_cast<char>(a*16+b);
    }
    return std::filesystem::u8path(decoded);
}
inline CoreLoadOptions desktop_core_load_options(const DesktopFirmwareSettings& settings,
                                                const std::string& hardware,
                                                const DesktopFirmwareSettings& cli={}) {
    const auto& selected=cli.enabled?cli:settings;
    CoreLoadOptions options;
    options.hardware_model=selected.enabled?selected.model:hardware;
    if(selected.enabled) {
        validate_desktop_firmware(selected);
        options.sgb_firmware_directory=selected.directory;
    }
    return options;
}
}
