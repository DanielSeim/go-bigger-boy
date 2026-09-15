#include "remote_link_controller.hpp"

#include "dialogs.hpp"
#include "emulation_session.hpp"
#include "gbb/frontend_logging.hpp"

#ifdef __ANDROID__
#include "android_bridge.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <exception>

namespace gbb::sdl {

namespace {

void stop_remote_discovery(RemoteLinkSession& remote) noexcept {
    remote.discovery.stop();
    remote.scanning = false;
#ifdef __ANDROID__
    stop_android_lan_discovery();
#endif
}

} // namespace

void process_remote_link_requests(RemoteLinkControlContext context) {
    const auto now = std::chrono::steady_clock::now();

    if (context.remote_link.scanning) {
        context.remote_link.discovery.poll();
        if (now >= context.remote_link.scan_deadline) {
            const auto peers = context.remote_link.discovery.take_peers();
            stop_remote_discovery(context.remote_link);
            if (peers.empty()) {
                show_error(
                    context.sdl.window,
                    "No compatible GBB link hosts were found on the LAN. "
                    "Enable LAN discovery on the host and allow UDP port 8764.");
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
            "Remote link rejected: peer compatibility profile does not match");
        stop_remote_link_session(*context.emulator, context.remote_link);
        show_error(context.sdl.window,
                   "The remote link was rejected because the ROM versions are not compatible.");
    }

    if (context.emulator != nullptr && context.remote_link.active() &&
        context.remote_link.active_channel().state() ==
            gameboy::LinkPacketChannel::State::failed &&
        !context.remote_link.failure_reported) {
        context.remote_link.failure_reported = true;
        gbb::log_frontend_warning(
            "Remote link lost; serial state was reset and retry is available");
        show_error(
            context.sdl.window,
            context.remote_link.endpoint.failure_during_transfer()
                ? "The remote link was interrupted during a serial transfer. "
                  "Automatic reconnect is disabled to prevent desync. Confirm "
                  "both games are ready, then choose Emulation > Retry Link Handshake."
                : "The remote link was lost. The serial transfer was reset. "
                  "Choose Emulation > Retry Link Handshake to reconnect.");
    }

    if (context.emulator != nullptr && context.remote_link.active()) {
        const auto state = context.remote_link.active_channel().state();
        if ((state == gameboy::LinkPacketChannel::State::failed ||
             state == gameboy::LinkPacketChannel::State::disconnected) &&
            !context.remote_link.endpoint.failure_during_transfer() &&
            context.remote_link.automatic_retry_attempts < 3) {
            if (context.remote_link.next_automatic_retry ==
                std::chrono::steady_clock::time_point{}) {
                context.remote_link.next_automatic_retry =
                    now + std::chrono::seconds(1);
            } else if (now >= context.remote_link.next_automatic_retry) {
                ++context.remote_link.automatic_retry_attempts;
                const auto delay = 1U << std::min(
                    context.remote_link.automatic_retry_attempts, 2U);
                context.remote_link.next_automatic_retry =
                    now + std::chrono::seconds(delay);
                try {
                    retry_remote_link_session(*context.emulator,
                                              context.remote_link,
                                              context.remote_options);
                    gbb::log_frontend_info("Remote link automatically reconnected");
                } catch (const std::exception& error) {
                    gbb::log_frontend_warning(
                        std::string("Automatic remote link retry failed: ") +
                        error.what());
                }
            }
        }
    }

    if (context.remote_discover_requested) {
        context.remote_discover_requested = false;
        if (context.emulator == nullptr) {
            show_error(context.sdl.window,
                       "Load a ROM before searching for LAN link hosts.");
        } else if (context.remote_options.transport != "tcp") {
            show_error(context.sdl.window,
                       "LAN discovery is available only for TCP links. "
                       "Choose a paired Bluetooth device in Link settings.");
        } else if (!context.remote_link.scanning) {
#ifdef __ANDROID__
            // Android filters multicast and broadcast packets unless the
            // Wi-Fi multicast lock is held. Acquire it before opening the
            // scanner socket so replies are receivable for the full bounded
            // scan, not just while hosting a session.
            if (!start_android_lan_discovery()) {
                show_error(
                    context.sdl.window,
                    "Could not enable Android LAN discovery. Allow Nearby devices "
                    "and retry the link action.");
            } else
#endif
            if (!context.remote_link.discovery.start_scan(
                    context.emulator->link_compatibility_id(),
                    context.emulator->rom_fingerprint(),
                    context.emulator->link_compatibility_profile())) {
#ifdef __ANDROID__
                stop_android_lan_discovery();
#endif
                show_error(context.sdl.window,
                           "Could not start LAN link discovery.");
            } else {
                context.remote_link.scanning = true;
                context.remote_link.scan_deadline =
                    now + std::chrono::milliseconds(2000);
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
                    stop_remote_discovery(context.remote_link);
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
                gbb::log_frontend_info(
                    context.remote_link.bluetooth
                        ? (hosting ? "Bluetooth link host started"
                                   : "Bluetooth link join started")
                        : (hosting ? "TCP link host started"
                                   : "TCP link join started"));
            } catch (const std::exception& error) {
                show_error(context.sdl.window, error.what());
            }
        }
    }

    if (context.link_retry_requested) {
        context.link_retry_requested = false;
        if (context.emulator != nullptr && context.remote_link.active()) {
            try {
                context.remote_link.automatic_retry_attempts = 0;
                context.remote_link.next_automatic_retry = {};
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
