#pragma once

#include "core_capability.hpp"
#include "remote_link_session.hpp"
#include "sdl_resources.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <deque>
#include <vector>

namespace gbb::sdl {

struct RemoteLinkControlContext final {
    CoreServices services;
    gameboy::Emulator* emulator{};
    RemoteLinkSession& remote_link;
    RemoteLinkOptions& remote_options;
    SdlResources& sdl;
    const std::filesystem::path& preference_path;
    bool link_diagnostics{};
    bool& remote_stop_requested;
    bool& remote_host_requested;
    bool& remote_join_requested;
    bool& remote_discover_requested;
    bool& link_retry_requested;
    bool& rewind;
    std::deque<std::vector<std::uint8_t>>& rewind_history;
};

// Android's touch menu and the desktop menu both feed the same request flags.
// This controller is deliberately transport-only: it never owns the core and
// never blocks while waiting for a LAN response.
void process_remote_link_requests(RemoteLinkControlContext context);

} // namespace gbb::sdl
