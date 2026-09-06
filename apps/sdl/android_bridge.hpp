#pragma once

#ifdef __ANDROID__

#include "gbb/log.hpp"

#include <optional>
#include <string>

namespace gbb::sdl {

struct AndroidRomRequest {
    std::string path;
    std::string display_name;
    gbb::LogContext log_context{};
};

[[nodiscard]] std::optional<AndroidRomRequest> take_android_rom_request() noexcept;
[[nodiscard]] std::optional<gbb::LogContext>
take_android_back_request() noexcept;
void publish_android_log_context(gbb::LogContext context) noexcept;
void request_android_back() noexcept;
void request_android_rom(AndroidRomRequest request);
void open_android_link_settings() noexcept;
[[nodiscard]] bool take_android_link_settings_changed() noexcept;
// Enables Wi-Fi multicast/broadcast reception while LAN discovery is active.
// The implementation is Android-only; callers must release it when the
// discovery socket is stopped so the radio can return to its normal filter.
[[nodiscard]] bool start_android_lan_discovery() noexcept;
void stop_android_lan_discovery() noexcept;

} // namespace gbb::sdl

#endif
