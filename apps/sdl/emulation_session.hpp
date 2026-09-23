#pragma once

#include "gameboy/display_palette.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/gameboy_link_endpoint.hpp"
#include "gameboy/link_session.hpp"
#include "remote_link_session.hpp"
#include "sdl_resources.hpp"
#include "gbb/core_registry.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gbb::sdl {

bool configure_video_pipeline(SdlResources& sdl, gameboy::VideoMode mode);
bool restore_video_presentation(const SdlResources& sdl);

void load_rom(const std::string& path,
              std::unique_ptr<gbb::EmulatorCore>& core,
              const gbb::CoreRegistry& registry,
              const gameboy::DisplayPalette& palette,
              SdlResources& sdl,
              const std::filesystem::path& preference_path,
              std::string hardware_model = "auto");

void start_link_trace(const std::filesystem::path& preference_path,
                      const char* role_suffix = nullptr,
                      const char* transport = nullptr);
void stop_link_trace() noexcept;

// Append a structured lifecycle event to the active link trace. This is a
// no-op when diagnostics are disabled, so callers can instrument failure
// paths without threading trace ownership through the emulation loop.
void trace_link_event(std::string_view event,
                      std::string_view fields = {}) noexcept;

// Android exports the active or most recently completed trace through the
// system file picker. The snapshot is empty when diagnostics were disabled or
// no trace has been written yet.
[[nodiscard]] std::vector<std::uint8_t> read_link_trace() noexcept;

#ifndef __ANDROID__
void trace_link_frame(gameboy::Emulator& first,
                      gameboy::Emulator& second,
                      int audio_queued_bytes);
#endif

// Scheduler facts for one remote-link video frame. These are emitted only in
// an enabled link trace, so collecting them does not add normal-run I/O.
struct RemoteFrameMetrics {
    std::uint32_t slices{};
    std::uint32_t polling_slices{};
    std::uint32_t idle_slices{};
    std::uint32_t endpoint_polls{};
    std::uint64_t emulated_cycles{};
    std::uint64_t polling_cycles{};
    std::uint32_t minimum_interval{};
    std::uint32_t maximum_interval{};
};

void trace_remote_frame(gameboy::Emulator& emulator,
                        const RemoteLinkSession& remote,
                        int audio_queued_bytes,
                        const RemoteFrameMetrics& metrics = {});

#ifndef __ANDROID__
void start_local_link_session(
    const std::string& path, gameboy::Emulator& first,
    std::unique_ptr<gameboy::Emulator>& second,
    std::unique_ptr<gameboy::LinkSession>& session,
    std::unique_ptr<gameboy::GameBoyLinkEndpoint>& first_endpoint,
    std::unique_ptr<gameboy::GameBoyLinkEndpoint>& second_endpoint,
    SdlResources& sdl, const gameboy::DisplayPalette& palette,
    const std::filesystem::path& preference_path,
    bool link_diagnostics);

void stop_local_link_session(
    gameboy::Emulator& first,
    std::unique_ptr<gameboy::Emulator>& second,
    std::unique_ptr<gameboy::LinkSession>& session,
    std::unique_ptr<gameboy::GameBoyLinkEndpoint>& first_endpoint,
    std::unique_ptr<gameboy::GameBoyLinkEndpoint>& second_endpoint,
    SdlResources& sdl,
    const std::filesystem::path& preference_path = {}) noexcept;

void retry_local_link_session(gameboy::Emulator& first,
                              gameboy::Emulator& second,
                              gameboy::LinkSession& session) noexcept;
#endif

void start_remote_link_session(gameboy::Emulator& emulator,
                               RemoteLinkSession& remote,
                               const RemoteLinkOptions& options,
                               bool hosting,
                               const std::filesystem::path& preference_path,
                               bool link_diagnostics,
                               SDL_Window* window);
void stop_remote_link_session(gameboy::Emulator& emulator,
                              RemoteLinkSession& remote,
                              const std::filesystem::path& preference_path = {}) noexcept;
void retry_remote_link_session(gameboy::Emulator& emulator,
                               RemoteLinkSession& remote,
                               const RemoteLinkOptions& options);

} // namespace gbb::sdl
