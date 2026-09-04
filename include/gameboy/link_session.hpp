#pragma once

#include "gameboy/link_endpoint.hpp"
#include "gameboy/link_transport.hpp"
#include "gbb/link_scheduler.hpp"

#include <cstdint>

namespace gameboy {

// Coordinates two endpoints attached to one deterministic local cable.  The
// frontend owns the core objects (and their save-state policy), while the
// session owns the cable and the cycle-balanced scheduler used to run them.
class LinkSession {
public:
    enum class State {
        disconnected,
        starting,
        connected,
        transferring,
        timed_out,
    };

    LinkSession() noexcept = default;
    explicit LinkSession(std::uint64_t timeout_cycles) noexcept
        : watchdog_(timeout_cycles) {}
    explicit LinkSession(LinkTransport& transport,
                         std::uint64_t timeout_cycles =
                             default_timeout_cycles) noexcept
        : transport_(&transport), watchdog_(timeout_cycles) {}
    LinkSession(const LinkSession&) = delete;
    LinkSession& operator=(const LinkSession&) = delete;
    ~LinkSession() { stop(); }

    void start(LinkEndpoint& first, LinkEndpoint& second) noexcept;
    void stop() noexcept;
    void retry() noexcept;

    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::uint64_t transfers_completed() const noexcept;

    // Advance both machines by approximately target_cycles, keeping their
    // cycle totals balanced so serial interrupts cannot be starved by a host
    // scheduler that happens to run one frontend slice first.
    void advance(unsigned target_cycles);

    // Reserved for a frontend that detects a guest-level link timeout. The
    // cable remains attached so the caller can display the failure and then
    // decide whether to retry or stop the session.
    void mark_timeout() noexcept;

    static constexpr std::uint64_t default_timeout_cycles =
        UINT64_C(4194304) * 8U;

private:
    LinkEndpoint* first_{};
    LinkEndpoint* second_{};
    LocalLinkTransport local_transport_{};
    LinkTransport* transport_{&local_transport_};
    State state_{State::disconnected};
    gbb::LinkWatchdog watchdog_{default_timeout_cycles};
    std::uint64_t diagnostic_session_{};
};

} // namespace gameboy
