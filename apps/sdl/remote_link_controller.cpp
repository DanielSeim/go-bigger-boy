#include "remote_link_controller.hpp"

#include "dialogs.hpp"
#include "emulation_session.hpp"
#include "gbb/frontend_logging.hpp"

#include <chrono>
#include <exception>

namespace gbb::sdl {

void process_remote_link_requests(RemoteLinkControlContext context) {
    const auto now = std::chrono::steady_clock::now();

    if (context.remote_link.scanning) {
        context.remote_link.discovery.poll();
        if (now >= context.remote_link.scan_deadline) {
            const auto peers = context.remote_link.discovery.take_peers();
            context.remote_link.discovery.stop();
            context.remote_link.scanning = false;
            if (peers.empty()) {
                show_error(context.sdl.window,
                           "No compatible GBB link hosts were found on the LAN.");
            } else {
                context.remote_options.host = peers.front().address;
                context.remote_options.port = peers.front().port;
                show_lan_hosts(context.sdl.window, peers);
            }
        }
    }

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
        } else if (!context.remote_link.scanning) {
            if (!context.remote_link.discovery.start_scan(
                    context.emulator->link_compatibility_id(),
                    context.emulator->rom_fingerprint())) {
                show_error(context.sdl.window,
                           "Could not start LAN link discovery.");
            } else {
                context.remote_link.scanning = true;
                context.remote_link.scan_deadline =
                    now + std::chrono::milliseconds(600);
                gbb::log_frontend_info("LAN link discovery started");
            }
        }
    }

    if (context.remote_stop_requested) {
        context.remote_stop_requested = false;
        if (context.emulator != nullptr && context.remote_link.active()) {
            gbb::log_frontend_info("Link request: stop remote session");
            stop_remote_link_session(*context.emulator, context.remote_link);
        }
    }

    if (context.remote_host_requested || context.remote_join_requested) {
        const auto hosting = context.remote_host_requested;
        context.remote_host_requested = false;
        context.remote_join_requested = false;
        if (context.emulator == nullptr || context.services.link_cable() == nullptr) {
            show_error(context.sdl.window,
                       "The loaded core does not provide a link-cable service.");
        } else {
            try {
                if (context.remote_link.scanning) {
                    context.remote_link.discovery.stop();
                    context.remote_link.scanning = false;
                }
                if (context.remote_link.active()) {
                    stop_remote_link_session(*context.emulator,
                                             context.remote_link);
                }
                start_remote_link_session(
                    *context.emulator, context.remote_link, context.remote_options,
                    hosting, context.preference_path, context.link_diagnostics,
                    context.sdl.window);
                context.rewind = false;
                context.rewind_history.clear();
                gbb::log_frontend_info(hosting
                                           ? "TCP link host started"
                                           : "TCP link join started");
            } catch (const std::exception& error) {
                show_error(context.sdl.window, error.what());
            }
        }
    }

    if (context.link_retry_requested) {
        context.link_retry_requested = false;
        if (context.emulator != nullptr && context.remote_link.active()) {
            try {
                retry_remote_link_session(*context.emulator,
                                          context.remote_link,
                                          context.remote_options);
            } catch (const std::exception& error) {
                show_error(context.sdl.window, error.what());
            }
        }
    }
}

} // namespace gbb::sdl
