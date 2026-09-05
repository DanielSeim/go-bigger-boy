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

namespace gbb::sdl {

bool configure_video_pipeline(SdlResources& sdl, gameboy::VideoMode mode);
bool restore_video_presentation(const SdlResources& sdl);

void load_rom(const std::string& path,
              std::unique_ptr<gbb::EmulatorCore>& core,
              const gbb::CoreRegistry& registry,
              const gameboy::DisplayPalette& palette,
              SdlResources& sdl,
              const std::filesystem::path& preference_path);

#ifndef __ANDROID__
void start_link_trace(const std::filesystem::path& preference_path,
                      const char* role_suffix = nullptr);
void stop_link_trace() noexcept;

void trace_link_frame(const gameboy::Emulator& first,
                      const gameboy::Emulator& second,
                      int audio_queued_bytes);
void trace_remote_frame(const gameboy::Emulator& emulator,
                        const RemoteLinkSession& remote,
                        int audio_queued_bytes);

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
    SdlResources& sdl) noexcept;

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
                              RemoteLinkSession& remote) noexcept;
void retry_remote_link_session(gameboy::Emulator& emulator,
                               RemoteLinkSession& remote,
                               const RemoteLinkOptions& options);

} // namespace gbb::sdl
