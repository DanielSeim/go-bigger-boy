#include "gameboy/emulator.hpp"
#include "gameboy/gameboy_link_endpoint.hpp"
#include "gameboy/link_session.hpp"
#include "gameboy/tcp_link_channel.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// The program is deliberately tiny, but it is executed by the real CPU and
// memory bus. The host loads its payload and starts one internal-clock
// transfer; the peer loads a different payload and waits on the external
// clock. The two synthetic images represent compatible games that share a
// caller-provided link identity.
std::vector<std::uint8_t> link_rom(const std::uint8_t payload,
                                   const bool internal_clock) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "LINK E2E TEST";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    const std::uint8_t program[] = {
        0x3E, payload,     // LD A,payload
        0xEA, 0x01, 0xFF, // LD (FF01),A
        0x3E, static_cast<std::uint8_t>(internal_clock ? 0x81 : 0x80),
                           // LD A,$81 (start, internal) or $80 (external)
        0xEA, 0x02, 0xFF, // LD (FF02),A
        0x18, 0xFE,       // JR $010B
    };
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x100);
    return rom;
}

gameboy::Emulator make_emulator(const std::uint8_t payload,
                                const bool internal_clock) {
    return gameboy::Emulator{
        gameboy::Cartridge{link_rom(payload, internal_clock)}};
}

void check_exchange(const gameboy::Emulator& first,
                    const gameboy::Emulator& second,
                    const std::uint8_t first_value,
                    const std::uint8_t second_value,
                    const std::uint64_t expected_transfers,
                    const char* transport) {
    const auto& first_serial = first.bus().serial_port();
    const auto& second_serial = second.bus().serial_port();
    const std::string prefix = std::string{transport} + " end-to-end";
    check(first_serial.transfers_completed() == expected_transfers &&
              second_serial.transfers_completed() == expected_transfers,
          (prefix + " completes the expected transfers on both emulators")
              .c_str());
    const auto bytes_match = first_serial.last_transmitted() == first_value &&
                             first_serial.last_received() == second_value &&
                             second_serial.last_transmitted() == second_value &&
                             second_serial.last_received() == first_value;
    if (!bytes_match) {
        std::cerr << "FAIL: " << prefix
                  << " preserves both transmitted and received bytes"
                  << " (p1 tx=" << static_cast<unsigned>(first_serial.last_transmitted())
                  << " rx=" << static_cast<unsigned>(first_serial.last_received())
                  << ", p2 tx=" << static_cast<unsigned>(second_serial.last_transmitted())
                  << " rx=" << static_cast<unsigned>(second_serial.last_received())
                  << ")\n";
        ++failures;
    }
}

void report_tcp_failure(const gameboy::TcpLinkChannel& server,
                        const gameboy::TcpLinkChannel& client,
                        const gameboy::TcpSerialEndpoint& server_endpoint,
                        const gameboy::TcpSerialEndpoint& client_endpoint,
                        const gameboy::Emulator& first,
                        const gameboy::Emulator& second) {
    const auto& first_serial = first.bus().serial_port();
    const auto& second_serial = second.bus().serial_port();
    std::cerr << "TCP diagnostics: channel(server="
              << static_cast<int>(server.state()) << ",client="
              << static_cast<int>(client.state()) << ") hello(server="
              << server_endpoint.peer_hello_seen() << ",client="
              << client_endpoint.peer_hello_seen() << ") compatible(server="
              << server_endpoint.peer_compatible() << ",client="
              << client_endpoint.peer_compatible() << ") transfers(server="
              << second_serial.transfers_completed() << ",client="
              << first_serial.transfers_completed() << ") requests(server="
              << server_endpoint.requests_received() << ",client="
              << client_endpoint.requests_received() << ") responses(server="
              << server_endpoint.responses_sent() << ",client="
              << client_endpoint.responses_sent() << ") malformed(server="
              << server.malformed_packets() << ",client="
              << client.malformed_packets() << ")\n";
}

void test_local_session() {
    auto first = make_emulator(0xA5, true);
    auto second = make_emulator(0x3C, false);
    gameboy::GameBoyLinkEndpoint first_endpoint{first};
    gameboy::GameBoyLinkEndpoint second_endpoint{second};
    gameboy::LinkSession session;
    session.start(first_endpoint, second_endpoint);

    for (unsigned attempt = 0;
         attempt < 32 && session.transfers_completed() < 2; ++attempt) {
        session.advance(1024);
    }
    check(session.active(), "local end-to-end session remains active");
    check_exchange(first, second, 0xA5, 0x3C, 1, "local");
    session.stop();
}

#if defined(__ANDROID__)

bool test_tcp_session() {
    // Android builds can run the deterministic local-cable test directly.
    // Device-to-device TCP requires an instrumentation runner (and two
    // devices), so it is intentionally covered by the desktop socket test.
    return true;
}

#else

bool test_tcp_session() {
    auto first = make_emulator(0xA5, true);
    auto second = make_emulator(0x3C, false);
    gameboy::TcpLinkChannel server;
    gameboy::TcpLinkChannel client;
    if (!server.listen(0) || server.local_port() == 0 ||
        !client.connect("127.0.0.1", server.local_port())) {
        std::cerr << "SKIP: TCP loopback is unavailable in this environment\n";
        return false;
    }

    // Establish the transport before attaching the serial endpoints.  The
    // endpoint's hello exchange is intentionally non-blocking, so attaching
    // while the socket is still connecting can otherwise leave the first
    // handshake part queued behind a platform-specific connect notification
    // (notably on macOS).  Waiting here keeps this deterministic fixture
    // focused on the protocol rather than socket-establishment timing.
    const auto connection_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < connection_deadline &&
           (server.state() != gameboy::TcpLinkChannel::State::connected ||
            client.state() != gameboy::TcpLinkChannel::State::connected)) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (server.state() != gameboy::TcpLinkChannel::State::connected ||
        client.state() != gameboy::TcpLinkChannel::State::connected) {
        std::cerr << "SKIP: TCP loopback did not establish a peer\n";
        return false;
    }

    gameboy::TcpSerialEndpoint first_endpoint;
    gameboy::TcpSerialEndpoint second_endpoint;
    first_endpoint.set_arbitration_priority(true);
    second_endpoint.set_arbitration_priority(false);
    // Different synthetic payloads intentionally have different ROM
    // fingerprints. A shared identity models two compatible releases and
    // exercises the same negotiated handshake used by regional Pokémon
    // variants.
    constexpr std::uint64_t shared_link_id = UINT64_C(0x454E44544F454E44);
    const auto profile = first.link_compatibility_profile();
    first_endpoint.attach(first.bus().serial_port(), client, shared_link_id,
                          profile);
    second_endpoint.attach(second.bus().serial_port(), server, shared_link_id,
                           profile);

    const auto run_guest_transfer = [&](const std::uint8_t first_value,
                                        const std::uint8_t second_value,
                                        const bool first_internal) {
        const auto first_target =
            first.bus().serial_port().transfers_completed() + 1;
        const auto second_target =
            second.bus().serial_port().transfers_completed() + 1;
        first.bus().write8(0xFF01, first_value);
        second.bus().write8(0xFF01, second_value);
        if (first_internal) {
            second.bus().write8(0xFF02, 0x80);
            first.bus().write8(0xFF02, 0x81);
        } else {
            first.bus().write8(0xFF02, 0x80);
            second.bus().write8(0xFF02, 0x81);
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline &&
               (first.bus().serial_port().transfers_completed() < first_target ||
                second.bus().serial_port().transfers_completed() < second_target)) {
            first_endpoint.poll();
            second_endpoint.poll();
            // Keep both guest clocks moving while the endpoint services
            // non-blocking sockets. This is the same balanced cadence used by
            // the production remote-link scheduler.
            if (first.cpu().total_cycles() <= second.cpu().total_cycles()) {
                static_cast<void>(first.step());
            } else {
                static_cast<void>(second.step());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        first_endpoint.poll();
        second_endpoint.poll();
        return first.bus().serial_port().transfers_completed() >= first_target &&
               second.bus().serial_port().transfers_completed() >= second_target;
    };

    // Let the synthetic guests perform their first transfer from their ROM
    // entry points. This mirrors production startup exactly and avoids
    // overwriting a guest's initial SB/SC writes with test-side registers.
    const auto initial_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < initial_deadline &&
           (first.bus().serial_port().transfers_completed() < 1 ||
            second.bus().serial_port().transfers_completed() < 1)) {
        first_endpoint.poll();
        second_endpoint.poll();
        if (first.cpu().total_cycles() <= second.cpu().total_cycles()) {
            static_cast<void>(first.step());
        } else {
            static_cast<void>(second.step());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    first_endpoint.poll();
    second_endpoint.poll();
    if (first.bus().serial_port().transfers_completed() < 1 ||
        second.bus().serial_port().transfers_completed() < 1) {
        report_tcp_failure(server, client, second_endpoint, first_endpoint,
                           first, second);
    }
    check(first.bus().serial_port().transfers_completed() >= 1 &&
              second.bus().serial_port().transfers_completed() >= 1,
          "TCP end-to-end completes the initial guest transfer");

    // Continue with alternating clock ownership and payloads. A single
    // successful byte can hide ownership drift that appears after Pokémon's
    // Cable Club handshake; this sustained exchange exercises the same
    // repeated battle/trade traffic while retaining real CPU execution.
    for (unsigned transfer = 1; transfer < 8; ++transfer) {
        const auto first_value = static_cast<std::uint8_t>(0xA5U + transfer);
        const auto second_value = static_cast<std::uint8_t>(0x3CU + transfer);
        if (!run_guest_transfer(first_value, second_value,
                                (transfer & 1U) == 0)) {
            report_tcp_failure(server, client, second_endpoint, first_endpoint,
                               first, second);
            break;
        }
        check(first.bus().serial_port().last_transmitted() == first_value &&
                  first.bus().serial_port().last_received() == second_value &&
                  second.bus().serial_port().last_transmitted() == second_value &&
                  second.bus().serial_port().last_received() == first_value,
              "TCP end-to-end preserves alternating guest payloads");
    }

    first_endpoint.poll();
    second_endpoint.poll();
    check(first_endpoint.peer_hello_seen() && second_endpoint.peer_hello_seen(),
          "TCP end-to-end peers complete the compatibility handshake");
    check(first_endpoint.peer_compatible() && second_endpoint.peer_compatible(),
          "TCP end-to-end peers accept matching compatibility identities");
    check_exchange(first, second, 0xAC, 0x43, 8, "TCP");
    first_endpoint.detach();
    second_endpoint.detach();
    return true;
}

#endif

} // namespace

int main(const int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "all";
    if (mode != "all" && mode != "local" && mode != "tcp") {
        std::cerr << "Usage: gameboy_link_end_to_end_tests [all|local|tcp]\n";
        return 2;
    }
    if (mode != "tcp") test_local_session();
    const auto tcp_ran = mode == "local" ? true : test_tcp_session();
    if (!tcp_ran && failures == 0) return 77;
    return failures == 0 ? 0 : 1;
}
