#include "link_controller.hpp"

#ifndef __ANDROID__

#include "dialogs.hpp"
#include "gbb/frontend_logging.hpp"

#include <exception>

namespace gbb::sdl {

void process_link_requests(LinkControlContext context) {
    if (context.emulator != nullptr && context.remote_link.active() &&
        context.remote_link.endpoint.peer_hello_seen() &&
        !context.remote_link.endpoint.peer_compatible()) {
        gbb::log_frontend_warning(
            "TCP link rejected: peer compatibility profile does not match");
        stop_remote_link_session(*context.emulator, context.remote_link);
        show_error(context.sdl.window,
                   "The remote link was rejected because the ROM versions are not compatible.");
    }

    if (context.remote_discover_requested) {
        context.remote_discover_requested = false;
        if (context.emulator == nullptr) {
            show_error(context.sdl.window,
                       "Load a ROM before searching for LAN link hosts.");
        } else {
            gameboy::LanDiscovery scanner;
            if (!scanner.start_scan(context.emulator->link_compatibility_id(),
                                    context.emulator->rom_fingerprint())) {
                show_error(context.sdl.window,
                           "Could not start LAN link discovery.");
            } else {
                // Discovery is a user-invoked, bounded scan. It never blocks
                // the emulation thread or opens a TCP session by itself.
                for (unsigned attempt = 0; attempt < 100; ++attempt) {
                    scanner.poll();
                    SDL_Delay(5);
                }
                scanner.poll();
                const auto peers = scanner.take_peers();
                scanner.stop();
                if (!peers.empty()) {
                    // Keep the discovery flow one step: the next Join
                    // command uses the first matching peer without requiring
                    // users to transcribe an address from the dialog.
                    context.remote_options.host = peers.front().address;
                    context.remote_options.port = peers.front().port;
                }
                show_lan_hosts(context.sdl.window, peers);
            }
        }
    }

    if (context.remote_stop_requested) {
        context.remote_stop_requested = false;
        gbb::log_frontend_info("Link request: stop remote session");
        if (context.emulator != nullptr && context.remote_link.active()) {
            stop_remote_link_session(*context.emulator, context.remote_link);
        }
    }

    if (context.remote_host_requested || context.remote_join_requested) {
        const auto hosting = context.remote_host_requested;
        context.remote_host_requested = false;
        context.remote_join_requested = false;
        gbb::log_frontend_info(hosting
                                   ? "Link request: host remote session"
                                   : "Link request: join remote session");
        try {
            if (context.link_emulator != nullptr) {
                stop_local_link_session(
                    *context.emulator, context.link_emulator,
                    context.link_session, context.link_first_endpoint,
                    context.link_second_endpoint, context.sdl);
            }
            if (context.remote_link.active()) {
                stop_remote_link_session(*context.emulator,
                                         context.remote_link);
            }
            auto* const link_emulator =
                context.services.link_cable();
            if (link_emulator == nullptr) {
                gbb::log_frontend_warning(
                    "Link request ignored: core has no link service");
            } else {
                start_remote_link_session(
                    *link_emulator, context.remote_link, context.remote_options,
                    hosting,
                    context.preference_path, context.link_diagnostics,
                    context.sdl.window);
                context.rewind = false;
                context.rewind_history.clear();
            }
        } catch (const std::exception& error) {
            show_error(context.sdl.window, error.what());
        }
    }

    if (context.link_retry_requested) {
        context.link_retry_requested = false;
        gbb::log_frontend_info("Link request: retry handshake");
        try {
            if (context.emulator != nullptr && context.remote_link.active()) {
                gbb::log_frontend_info("Link retry: remote transport");
                retry_remote_link_session(*context.emulator,
                                          context.remote_link,
                                          context.remote_options);
            } else if (context.emulator != nullptr &&
                       context.link_emulator != nullptr &&
                       context.link_session != nullptr) {
                gbb::log_frontend_info("Link retry: local cable");
                retry_local_link_session(*context.emulator,
                                         *context.link_emulator,
                                         *context.link_session);
                context.automatic_local_retry_used = false;
            }
        } catch (const std::exception& error) {
            show_error(context.sdl.window, error.what());
        }
    }

    // Recover one transient local handshake timeout, matching the manual
    // retry command without looping forever on an unavailable peer or ROM.
    if (context.emulator != nullptr && context.link_emulator != nullptr &&
        context.link_session != nullptr &&
        context.link_session->state() == gameboy::LinkSession::State::timed_out &&
        !context.automatic_local_retry_used) {
        gbb::log_frontend_warning(
            "Link session timed out; attempting one automatic retry");
        try {
            retry_local_link_session(*context.emulator,
                                     *context.link_emulator,
                                     *context.link_session);
            context.automatic_local_retry_used = true;
        } catch (const std::exception& error) {
            show_error(context.sdl.window, error.what());
            context.automatic_local_retry_used = true;
        }
    }

    if (context.link_toggle_requested) {
        context.link_toggle_requested = false;
        gbb::log_frontend_info("Link request: toggle local/remote session");
        try {
            if (context.remote_link.active()) {
                stop_remote_link_session(*context.emulator,
                                         context.remote_link);
            } else if (context.link_emulator != nullptr) {
                stop_local_link_session(
                    *context.emulator, context.link_emulator,
                    context.link_session, context.link_first_endpoint,
                    context.link_second_endpoint, context.sdl);
            } else if (context.services.link_cable() != nullptr) {
                start_local_link_session(
                    context.current_rom, *context.emulator,
                    context.link_emulator, context.link_session,
                    context.link_first_endpoint, context.link_second_endpoint,
                    context.sdl, context.palette, context.preference_path,
                    context.link_diagnostics);
                context.automatic_local_retry_used = false;
                context.rewind = false;
                context.rewind_history.clear();
            }
        } catch (const std::exception& error) {
            show_error(context.sdl.window, error.what());
        }
    }
    if (context.link_emulator == nullptr) {
        context.automatic_local_retry_used = false;
    }
}

} // namespace gbb::sdl

#endif // __ANDROID__
