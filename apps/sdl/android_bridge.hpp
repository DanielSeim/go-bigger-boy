#pragma once

#ifdef __ANDROID__

#include <optional>
#include <string>

namespace gbb::sdl {

struct AndroidRomRequest {
    std::string path;
    std::string display_name;
};

[[nodiscard]] std::optional<AndroidRomRequest> take_android_rom_request() noexcept;
[[nodiscard]] bool take_android_back_request() noexcept;
void request_android_back() noexcept;
void request_android_rom(AndroidRomRequest request);

} // namespace gbb::sdl

#endif
