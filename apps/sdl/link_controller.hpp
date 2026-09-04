#pragma once

#ifndef __ANDROID__

#include "core_capability.hpp"
#include "emulation_session.hpp"
#include "remote_link_session.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gbb::sdl {

struct LinkControlContext final {
    CoreServices services;
    gameboy::Emulator* emulator{};
    std::unique_ptr<gameboy::Emulator>& link_emulator;
    std::unique_ptr<gameboy::LinkSession>& link_session;
    std::unique_ptr<gameboy::GameBoyLinkEndpoint>& link_first_endpoint;
    std::unique_ptr<gameboy::GameBoyLinkEndpoint>& link_second_endpoint;
    RemoteLinkSession& remote_link;
    SdlResources& sdl;
    const std::string& current_rom;
    const gameboy::DisplayPalette& palette;
    const std::filesystem::path& preference_path;
    bool link_diagnostics{};
    std::deque<std::vector<std::uint8_t>>& rewind_history;
    bool& remote_stop_requested;
    bool& remote_host_requested;
    bool& remote_join_requested;
    bool& link_retry_requested;
    bool& link_toggle_requested;
    bool& automatic_local_retry_used;
    bool& rewind;
};

// Handles desktop link startup, retry, timeout recovery, and shutdown
// requests for one frontend iteration. Session construction remains in
// emulation_session; this module owns only request sequencing and policy.
void process_link_requests(LinkControlContext context);

} // namespace gbb::sdl

#endif // __ANDROID__
