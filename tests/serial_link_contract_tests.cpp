#include "gameboy/emulator.hpp"
#include "gameboy/link_session.hpp"
#include "gameboy/gameboy_link_endpoint.hpp"
#include "gameboy/link_endpoint.hpp"
#include "gameboy/link_transport.hpp"
#include "gameboy/tcp_link_channel.hpp"
#include "gameboy/tcp_serial_endpoint.hpp"
#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class CountingLinkEndpoint final : public gameboy::LinkEndpoint {
public:
    [[nodiscard]] gameboy::SerialPort& serial_port() noexcept override {
        return serial_port_;
    }

    [[nodiscard]] unsigned step() override {
        ++steps_;
        return 4;
    }

    [[nodiscard]] unsigned steps() const noexcept { return steps_; }

private:
    gameboy::SerialPort serial_port_;
    unsigned steps_{};
};

std::vector<std::uint8_t> test_rom(
    const std::vector<std::uint8_t>& program = {}) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "SERIAL TEST";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    std::copy(program.begin(), program.end(), rom.begin() + 0x100);
    return rom;
}

std::vector<std::uint8_t> cgb_test_rom() {
    auto rom = test_rom();
    rom[0x143] = 0x80;
    return rom;
}

void test_serial_transfer() {
    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.write8(0xFF0F, 0);
    bus.write8(0xFF01, 'A');
    bus.write8(0xFF02, 0x81);
    check(bus.read8(0xFF01) == 'A' && (bus.read8(0xFF02) & 0x80) != 0,
          "internal-clock serial transfer starts through SB/SC");
    bus.tick(4095);
    check(bus.take_serial_output().empty() &&
              (bus.read8(0xFF02) & 0x80) != 0,
          "serial transfer remains active before its eighth bit");
    bus.tick(1);
    check(bus.take_serial_output() == "A" &&
              (bus.read8(0xFF02) & 0x80) == 0 &&
              (bus.read8(0xFF0F) & 0x08) != 0,
          "serial transfer publishes its byte after eight clock edges");
    check(bus.take_serial_output().empty(),
          "taking serial output drains the observation buffer");

    bus.write8(0xFF0F, 0);
    bus.write8(0xFF01, 'B');
    bus.write8(0xFF02, 0x80); // External clock selected.
    bus.tick(8192);
    check(bus.take_serial_output().empty() &&
              (bus.read8(0xFF02) & 0x80) != 0 &&
              (bus.read8(0xFF0F) & 0x08) == 0,
          "external-clock serial transfer waits for an external peer");
}

void test_serial_link_cable() {
    gameboy::MemoryBus first{gameboy::Cartridge{test_rom()}};
    gameboy::MemoryBus second{gameboy::Cartridge{test_rom()}};
    gameboy::SerialCable cable;
    cable.connect(first.serial_port(), second.serial_port());

    first.write8(0xFF01, 0xA5);
    second.write8(0xFF01, 0x3C);
    first.write8(0xFF02, 0x81);  // Player one supplies the clock.
    second.write8(0xFF02, 0x80); // Player two uses the external clock.

    first.tick(511);
    check(first.read8(0xFF01) == 0xA5 && second.read8(0xFF01) == 0x3C,
          "linked serial ports wait for the first clock edge");
    first.tick(512);
    check(first.read8(0xFF01) == 0x4A && second.read8(0xFF01) == 0x79,
          "linked serial ports exchange one bit on each clock edge");
    first.tick(7 * 512);
    check(first.read8(0xFF01) == 0x3C && second.read8(0xFF01) == 0xA5 &&
              (first.read8(0xFF02) & 0x80) == 0 &&
              (second.read8(0xFF02) & 0x80) == 0 &&
              (first.read8(0xFF0F) & 0x08) != 0 &&
              (second.read8(0xFF0F) & 0x08) != 0,
          "linked serial transfers complete on both consoles");

    check(first.serial_port().transfers_completed() == 1 &&
              second.serial_port().transfers_completed() == 1,
          "linked serial diagnostics count completed transfers");
    first.serial_port().reset_diagnostics();
    second.serial_port().reset_diagnostics();
    check(first.serial_port().transfers_completed() == 0 &&
              second.serial_port().transfers_completed() == 0 &&
              first.serial_port().last_transmitted() == 0xFF &&
              first.serial_port().last_received() == 0xFF &&
              second.serial_port().last_transmitted() == 0xFF &&
              second.serial_port().last_received() == 0xFF,
          "serial diagnostics reset without changing link state");

    // Pokémon's Cable Club can have both consoles request the internal clock
    // during the same handshake window. A real cable has one clock source; the
    // first deterministic request wins, while the loser keeps its preceding
    // external probe byte available for the winning edge.
    first.write8(0xFF01, 0x02);
    second.write8(0xFF01, 0x02);
    first.write8(0xFF02, 0x80);
    second.write8(0xFF02, 0x80);
    first.write8(0xFF01, 0x01);
    second.write8(0xFF01, 0x01);
    first.write8(0xFF02, 0x81);
    second.write8(0xFF02, 0x81);
    check(first.serial_port().internal_clock() &&
              !second.serial_port().internal_clock(),
          "a linked cable arbitrates simultaneous internal-clock requests");
    first.tick(4096);
    check(first.read8(0xFF01) == 0x02 && second.read8(0xFF01) == 0x01 &&
              (first.read8(0xFF0F) & 0x08) != 0 &&
              (second.read8(0xFF0F) & 0x08) != 0,
          "arbitrated probe exchanges the preceding external byte");

    // Mirror the Gen I probe sequence: both sides arm external first, then
    // switch to internal to discover a peer that is already waiting.
    first.write8(0xFF01, 0x02);
    second.write8(0xFF01, 0x02);
    first.write8(0xFF02, 0x80);
    second.write8(0xFF02, 0x80);
    first.write8(0xFF02, 0x81);
    second.write8(0xFF02, 0x81);
    first.tick(4096);
    second.tick(4096);
    check(first.read8(0xFF01) == 0x02 && second.read8(0xFF01) == 0x02 &&
              (first.read8(0xFF0F) & 0x08) != 0 &&
              (second.read8(0xFF0F) & 0x08) != 0,
          "Gen I external-then-internal probe exchanges both role bytes");

    // A connected cable holds the first edge until its peer arms the receiver;
    // this prevents a startup probe from being consumed as pull-up bits.
    first.write8(0xFF0F, 0);
    second.write8(0xFF0F, 0);
    first.write8(0xFF01, 0x12);
    second.write8(0xFF01, 0x34);
    first.write8(0xFF02, 0x81);
    first.tick(4096);
    check(first.serial_port().transfer_active() &&
              first.serial_port().bits_shifted() == 0 &&
              (first.read8(0xFF0F) & 0x08) == 0,
          "connected internal clock waits for an unarmed external peer");
    second.write8(0xFF02, 0x80);
    // Arming the receiver releases only the next edge. Advance one bit at a
    // time so the test also guards against replaying the time spent waiting.
    for (unsigned bit = 0; bit < 8; ++bit) first.tick(512);
    check(!first.serial_port().transfer_active() &&
              !second.serial_port().transfer_active() &&
              first.read8(0xFF01) == 0x34 &&
              second.read8(0xFF01) == 0x12 &&
              (first.read8(0xFF0F) & 0x08) != 0 &&
              (second.read8(0xFF0F) & 0x08) != 0,
          "armed external peer receives the held transfer");

    // A peer edge is ignored while a port is the internal clock source; only
    // the cable owner's edge shifts both ports.
    second.write8(0xFF01, 0x80);
    second.write8(0xFF02, 0x81);
    static_cast<void>(second.serial_port().clock_external_bit(false));
    check(second.read8(0xFF01) == 0x80,
          "a cable edge does not shift an internal clock source");

    cable.disconnect();
    first.write8(0xFF0F, 0);
    first.write8(0xFF01, 0x00);
    first.write8(0xFF02, 0x81);
    first.tick(4096);
    check(first.read8(0xFF01) == 0xFF,
          "a disconnected link supplies pull-up one bits");

    gameboy::MemoryBus cgb{gameboy::Cartridge{cgb_test_rom()}};
    cgb.write8(0xFF01, 0x00);
    cgb.write8(0xFF02, 0x83);
    cgb.tick(127);
    check((cgb.read8(0xFF02) & 0x80) != 0,
          "CGB fast serial remains active before eight fast edges");
    cgb.tick(1);
    check((cgb.read8(0xFF02) & 0x80) == 0,
          "CGB fast serial completes after 128 CPU cycles");
}

void test_serial_link_interrupt_handshake() {
    // A tiny ROM-level probe matching Pokémon's external-then-internal
    // connection routine. The ISR copies the received SB byte into the HRAM
    // connection marker, exactly as the game does.
    const std::vector<std::uint8_t> program{
        0x31, 0xFE, 0xFF,       // LD SP,$FFFE
        0x3E, 0x08, 0xEA, 0xFF, 0xFF, // LD A,$08; LD ($FFFF),A
        0xFB,                   // EI
        0x3E, 0xFF, 0xEA, 0xAA, 0xFF, // marker = connection pending
        0x3E, 0x02, 0xEA, 0x01, 0xFF, // SB = external probe
        0x3E, 0x80, 0xEA, 0x02, 0xFF, // SC = external start
        0x3E, 0x01, 0xEA, 0x01, 0xFF, // SB = internal probe
        0x3E, 0x81, 0xEA, 0x02, 0xFF, // SC = internal start
        0x18, 0xFE};            // spin
    auto first_rom = test_rom(program);
    auto second_rom = first_rom;
    first_rom[0x58] = 0xF0;     // LDH A,($01)
    first_rom[0x59] = 0x01;
    first_rom[0x5A] = 0xEA;     // LD ($FFAA),A
    first_rom[0x5B] = 0xAA;
    first_rom[0x5C] = 0xFF;
    first_rom[0x5D] = 0xD9;     // RETI
    second_rom[0x58] = 0xF0;
    second_rom[0x59] = 0x01;
    second_rom[0x5A] = 0xEA;
    second_rom[0x5B] = 0xAA;
    second_rom[0x5C] = 0xFF;
    second_rom[0x5D] = 0xD9;
    gameboy::Emulator first{gameboy::Cartridge{std::move(first_rom)}};
    gameboy::Emulator second{gameboy::Cartridge{std::move(second_rom)}};
    gameboy::SerialCable cable;
    cable.connect(first.bus().serial_port(), second.bus().serial_port());
    for (unsigned instruction = 0; instruction < 20000; ++instruction) {
        static_cast<void>(first.step());
        static_cast<void>(second.step());
    }
    const auto first_status = first.bus().read8(0xFFAA);
    const auto second_status = second.bus().read8(0xFFAA);
    if (first_status != 0x02 || second_status != 0x01) {
        std::cerr << "probe status first=" << unsigned(first_status)
                  << " second=" << unsigned(second_status)
                  << " sb=" << unsigned(first.bus().read8(0xFF01)) << "/"
                  << unsigned(second.bus().read8(0xFF01)) << " sc="
                  << unsigned(first.bus().read8(0xFF02)) << "/"
                  << unsigned(second.bus().read8(0xFF02)) << '\n';
    }
    check(first_status == 0x02 && second_status == 0x01,
          "serial cable delivers probe bytes through ROM interrupt handlers");
}

void test_serial_link_interrupt_rearm() {
    // Continue the probe with repeated master/slave transfers. This models
    // the Cable Club's byte exchange where the external side must re-arm SC
    // from its serial ISR before the next internal edge.
    const std::vector<std::uint8_t> program{
        0x31, 0xFE, 0xFF, 0x3E, 0x08, 0xEA, 0xFF, 0xFF, 0xFB,
        0x3E, 0xFF, 0xEA, 0xAA, 0xFF, // pending marker
        0x3E, 0x02, 0xEA, 0x01, 0xFF, 0x3E, 0x80, 0xEA, 0x02, 0xFF,
        0x3E, 0x01, 0xEA, 0x01, 0xFF, 0x3E, 0x81, 0xEA, 0x02, 0xFF,
        0xF0, 0xAA, 0xFE, 0x02, 0x20, 0xFA, // wait for internal role
        0x3E, 0x60, 0xEA, 0x01, 0xFF, 0x3E, 0x81, 0xEA, 0x02, 0xFF,
        0x18, 0xEB}; // repeat internal transfers
    auto first_rom = test_rom(program);
    auto second_rom = first_rom;
    const std::array<std::uint8_t, 0x1C> isr{{
        0xF0, 0xAA, 0xFE, 0xFF, 0x20, 0x06, // if pending, capture SB
        0xF0, 0x01, 0xEA, 0xAA, 0xFF,       // hstatus = received probe
        0xF0, 0xAA, 0xFE, 0x01, 0x20, 0x0A, // if external, re-arm below
        0x3E, 0x60, 0xEA, 0x01, 0xFF, 0x3E, 0x80, 0xEA, 0x02, 0xFF,
        0xD9}};
    std::copy(isr.begin(), isr.end(), first_rom.begin() + 0x58);
    std::copy(isr.begin(), isr.end(), second_rom.begin() + 0x58);
    gameboy::Emulator first{gameboy::Cartridge{std::move(first_rom)}};
    gameboy::Emulator second{gameboy::Cartridge{std::move(second_rom)}};
    gameboy::SerialCable cable;
    cable.connect(first.bus().serial_port(), second.bus().serial_port());
    for (unsigned instruction = 0; instruction < 50000; ++instruction) {
        static_cast<void>(first.step());
        static_cast<void>(second.step());
    }
    check(first.bus().read8(0xFF01) == 0x60 &&
              second.bus().read8(0xFF01) == 0x60,
          "external serial ISR re-arms repeated linked transfers");
}

void test_serial_link_asymmetric_scheduling() {
    // The desktop frontend advances both machines in cycle-sized slices, but
    // host scheduling can still let one CPU run several instructions ahead.
    // Keep the same repeated-transfer probe as above while deliberately
    // starving each side in alternating bursts. The cable must hold an edge
    // until the peer arms its receiver and must not lose the next transfer.
    const std::vector<std::uint8_t> program{
        0x31, 0xFE, 0xFF, 0x3E, 0x08, 0xEA, 0xFF, 0xFF, 0xFB,
        0x3E, 0xFF, 0xEA, 0xAA, 0xFF, // pending marker
        0x3E, 0x02, 0xEA, 0x01, 0xFF, 0x3E, 0x80, 0xEA, 0x02, 0xFF,
        0x3E, 0x01, 0xEA, 0x01, 0xFF, 0x3E, 0x81, 0xEA, 0x02, 0xFF,
        0xF0, 0xAA, 0xFE, 0x02, 0x20, 0xFA, // wait for internal role
        0x3E, 0x60, 0xEA, 0x01, 0xFF, 0x3E, 0x81, 0xEA, 0x02, 0xFF,
        0x18, 0xEB};
    auto first_rom = test_rom(program);
    auto second_rom = first_rom;
    const std::array<std::uint8_t, 0x1C> isr{{
        0xF0, 0xAA, 0xFE, 0xFF, 0x20, 0x06,
        0xF0, 0x01, 0xEA, 0xAA, 0xFF,
        0xF0, 0xAA, 0xFE, 0x01, 0x20, 0x0A,
        0x3E, 0x60, 0xEA, 0x01, 0xFF, 0x3E, 0x80, 0xEA, 0x02, 0xFF,
        0xD9}};
    std::copy(isr.begin(), isr.end(), first_rom.begin() + 0x58);
    std::copy(isr.begin(), isr.end(), second_rom.begin() + 0x58);
    gameboy::Emulator first{gameboy::Cartridge{std::move(first_rom)}};
    gameboy::Emulator second{gameboy::Cartridge{std::move(second_rom)}};
    gameboy::SerialCable cable;
    cable.connect(first.bus().serial_port(), second.bus().serial_port());

    for (unsigned instruction = 0; instruction < 100000; ++instruction) {
        // Alternate which side gets a 4:1 share of host time. Both consoles
        // continue to make progress, but neither has lockstep instruction
        // timing to hide a peer-readiness bug.
        const auto first_ahead = (instruction / 32U) % 2U == 0;
        if (first_ahead) {
            static_cast<void>(first.step());
            if (instruction % 4U == 0) static_cast<void>(second.step());
        } else {
            if (instruction % 4U == 0) static_cast<void>(first.step());
            static_cast<void>(second.step());
        }
    }
    check(first.bus().read8(0xFF01) == 0x60 &&
              second.bus().read8(0xFF01) == 0x60,
          "linked transfers survive asymmetric emulator scheduling");
}

void test_link_session_lifecycle() {
    gameboy::Emulator first{gameboy::Cartridge{test_rom()}};
    gameboy::Emulator second{gameboy::Cartridge{test_rom()}};
    gameboy::LocalLinkTransport transport;
    gameboy::LinkSession session{transport};
    gameboy::GameBoyLinkEndpoint first_endpoint{first};
    gameboy::GameBoyLinkEndpoint second_endpoint{second};

    check(session.state() == gameboy::LinkSession::State::disconnected &&
              !session.active(),
          "link session starts disconnected");

    session.start(first_endpoint, second_endpoint);
    check(session.state() == gameboy::LinkSession::State::connected &&
              session.active() && first.bus().serial_port().has_endpoint() &&
              second.bus().serial_port().has_endpoint() &&
              transport.connected(),
          "link session attaches both serial endpoints");

    first.bus().write8(0xFF01, 0xA5);
    second.bus().write8(0xFF01, 0x5A);
    first.bus().write8(0xFF02, 0x81);
    second.bus().write8(0xFF02, 0x80);
    check(session.state() == gameboy::LinkSession::State::transferring,
          "link session reports an active serial transfer");
    session.advance(4096);
    check(first.bus().read8(0xFF01) == 0x5A &&
              second.bus().read8(0xFF01) == 0xA5 &&
              session.state() == gameboy::LinkSession::State::connected &&
              session.transfers_completed() == 2,
          "link session scheduler completes a transfer on both consoles");
    check(first_endpoint.emulated_cycles() != 0 &&
              second_endpoint.emulated_cycles() != 0,
          "link endpoints expose cycle positions for diagnostics");

    session.mark_timeout();
    check(session.state() == gameboy::LinkSession::State::timed_out &&
              !session.active() && first.bus().serial_port().has_endpoint() &&
              second.bus().serial_port().has_endpoint(),
          "link session exposes an explicit timeout state");
    session.stop();
    check(session.state() == gameboy::LinkSession::State::disconnected &&
              !first.bus().serial_port().has_endpoint() &&
              !second.bus().serial_port().has_endpoint() &&
              !transport.connected(),
          "link session detaches both endpoints on stop");

    session.start(first_endpoint, second_endpoint);
    check(session.active() && session.state() ==
              gameboy::LinkSession::State::connected,
          "link session can reconnect after stopping");
    session.stop();
}

void test_link_session_timeout_and_retry() {
    gameboy::Emulator first{gameboy::Cartridge{test_rom()}};
    gameboy::Emulator second{gameboy::Cartridge{test_rom()}};
    // Use a short watchdog budget so a stalled external receiver is covered
    // without spending seconds emulating an entire production timeout.
    gameboy::LinkSession session{1024};
    gameboy::GameBoyLinkEndpoint first_endpoint{first};
    gameboy::GameBoyLinkEndpoint second_endpoint{second};
    session.start(first_endpoint, second_endpoint);
    first.bus().write8(0xFF01, 0xA5);
    first.bus().write8(0xFF02, 0x81);
    session.advance(1024);
    check(session.state() == gameboy::LinkSession::State::timed_out &&
              !session.active(),
          "link session watchdog detects a stalled transfer");

    session.retry();
    check(session.state() == gameboy::LinkSession::State::connected &&
              session.active() && first.bus().serial_port().has_endpoint() &&
              second.bus().serial_port().has_endpoint() &&
              !first.bus().serial_port().transfer_active(),
          "link retry clears protocol state without replacing emulators");

    // A recovered session must be usable for the next byte, not merely report
    // a connected lifecycle state. This protects the trade/battle retry path
    // from regressing into a second deadlock after the watchdog fires.
    first.bus().write8(0xFF01, 0xA5);
    second.bus().write8(0xFF01, 0x5A);
    first.bus().write8(0xFF02, 0x81);
    second.bus().write8(0xFF02, 0x80);
    session.advance(4096);
    check(first.bus().read8(0xFF01) == 0x5A &&
              second.bus().read8(0xFF01) == 0xA5 &&
              session.state() == gameboy::LinkSession::State::connected &&
              session.transfers_completed() == 2,
          "link retry permits a fresh transfer after a timeout");
    session.stop();
}

void test_link_session_core_neutral_endpoint() {
    CountingLinkEndpoint first;
    CountingLinkEndpoint second;
    gameboy::LinkSession session;

    session.start(first, second);
    session.advance(64);
    check(first.steps() != 0 && second.steps() != 0,
          "link session schedules core-neutral endpoints");
    check(first.emulated_cycles() == 0 && second.emulated_cycles() == 0,
          "cycle diagnostics remain optional for core-neutral endpoints");
    session.stop();
}

void test_link_transport_framing() {
    const gameboy::LinkPacket packet{gameboy::LinkPacketType::bit,
                                     UINT32_C(0xA1B2C3D4), 0x5A, 0x03};
    const auto wire = gameboy::LinkPacketCodec::encode(packet);
    const auto decoded = gameboy::LinkPacketCodec::decode(wire.data(),
                                                            wire.size());
    check(decoded && decoded->type == packet.type &&
              decoded->sequence == packet.sequence &&
              decoded->value == packet.value && decoded->flags == packet.flags,
          "link transport framing round-trips packet fields");

    auto corrupted = wire;
    corrupted[8] ^= 0x01;
    check(!gameboy::LinkPacketCodec::decode(corrupted.data(), corrupted.size()),
          "link transport framing rejects a corrupted packet");
    check(!gameboy::LinkPacketCodec::decode(wire.data(), wire.size() - 1),
          "link transport framing rejects a truncated packet");
}

void test_tcp_link_channel_loopback() {
    gameboy::TcpLinkChannel server;
    gameboy::TcpLinkChannel client;
    const auto listening = server.listen(0) && server.local_port() != 0;
    // Some hermetic runners disable AF_INET sockets entirely. Keep the core
    // suite useful there; codec coverage still runs and desktop CI exercises
    // the channel with real sockets.
    if (!listening) return;
    check(server.state() == gameboy::TcpLinkChannel::State::listening,
          "TCP link channel listens on an ephemeral loopback port");
    check(client.connect("127.0.0.1", server.local_port()),
          "TCP link channel starts a non-blocking loopback connect");
    for (unsigned attempt = 0;
         attempt < 100 &&
         (server.state() != gameboy::TcpLinkChannel::State::connected ||
          client.state() != gameboy::TcpLinkChannel::State::connected);
         ++attempt) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(server.state() == gameboy::TcpLinkChannel::State::connected &&
              client.state() == gameboy::TcpLinkChannel::State::connected,
          "TCP link channel establishes a loopback peer without blocking");
    const gameboy::LinkPacket packet{gameboy::LinkPacketType::bit, 7, 0xA5,
                                     1};
    check(client.send(packet), "TCP link channel queues a framed packet");
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        client.poll();
        server.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto received = server.receive();
    check(received && received->sequence == packet.sequence &&
              received->value == packet.value && received->flags == packet.flags,
          "TCP link channel delivers framed packets over loopback");
}

void test_tcp_serial_endpoint_loopback() {
    gameboy::TcpLinkChannel server;
    gameboy::TcpLinkChannel client;
    if (!server.listen(0) || server.local_port() == 0) return;
    if (!client.connect("127.0.0.1", server.local_port())) return;
    for (unsigned attempt = 0;
         attempt < 100 &&
         (server.state() != gameboy::TcpLinkChannel::State::connected ||
          client.state() != gameboy::TcpLinkChannel::State::connected);
         ++attempt) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (server.state() != gameboy::TcpLinkChannel::State::connected ||
        client.state() != gameboy::TcpLinkChannel::State::connected) {
        return;
    }

    gameboy::MemoryBus first{gameboy::Cartridge{test_rom()}};
    gameboy::MemoryBus second{gameboy::Cartridge{test_rom()}};
    gameboy::TcpSerialEndpoint first_endpoint;
    gameboy::TcpSerialEndpoint second_endpoint;
    // Match the desktop TCP roles: the host (first endpoint) owns the
    // initial clock and the join side starts as the external receiver.
    first_endpoint.set_arbitration_priority(true);
    second_endpoint.set_arbitration_priority(false);
    first_endpoint.attach(first.serial_port(), client);
    second_endpoint.attach(second.serial_port(), server);

    // Exercise the reset path: leave one host bit pending, then have the
    // guest rewrite SC before the response arrives. A normal rewrite keeps
    // the request alive; an explicit link reset must cancel it cleanly.
    for (unsigned attempt = 0;
         attempt < 100 &&
         !first_endpoint.peer_ready_for_link();
         ++attempt) {
        first_endpoint.poll();
        second_endpoint.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    first.write8(0xFF01, 0xA5);
    second.write8(0xFF01, 0x5A);
    first.write8(0xFF02, 0x81);
    second.write8(0xFF02, 0x80);
    first.tick(512);
    check(first_endpoint.waiting_for_peer(),
          "TCP endpoint records an outstanding bit before serial reset");
    first.write8(0xFF02, 0x81);
    check(first_endpoint.waiting_for_peer(),
          "a normal SC rewrite preserves an in-flight TCP bit request");
    first.serial_port().reset_link();
    check(!first_endpoint.waiting_for_peer(),
          "explicit link reset cancels an obsolete TCP bit request");
    second.serial_port().reset_link();
    check(!first.serial_port().transfer_active(),
          "peer serial reset discards a partial external byte");
    // Let both channels consume the abandoned response and ordered reset
    // markers before the next guest arms SC. This is the same idle polling
    // cadence used by the desktop frontend between retries.
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        first_endpoint.poll();
        second_endpoint.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    first.write8(0xFF01, 0xA5);
    second.write8(0xFF01, 0x5A);
    first.write8(0xFF02, 0x81);
    second.write8(0xFF02, 0x81);
    for (unsigned cycle = 0; cycle < 20000; ++cycle) {
        first.tick(4);
        second.tick(4);
        // Match the desktop remote-link cadence (one network poll roughly
        // every 64 CPU cycles) instead of accidentally masking timing races
        // with a poll on every four-cycle tick.
        if ((cycle & 15U) == 0) {
            first_endpoint.poll();
            second_endpoint.poll();
        }
        if (!first.serial_port().transfer_active() &&
            !second.serial_port().transfer_active()) {
            break;
        }
    }
    check(first.read8(0xFF01) == 0x5A && second.read8(0xFF01) == 0xA5 &&
              !first.serial_port().transfer_active() &&
              !second.serial_port().transfer_active(),
          "TCP serial endpoint bridges a non-blocking loopback transfer");

    // A completed byte releases clock ownership. The join side must be able
    // to clock the following byte after receiving that release, which is the
    // sequence used by Pokémon when it re-enters the Cable Club after a
    // reset. Keep the host external for this transfer so the direction is
    // unambiguous.
    first.write8(0xFF01, 0x3C);
    second.write8(0xFF01, 0xC3);
    second.write8(0xFF02, 0x80);
    first.write8(0xFF02, 0x81);
    for (unsigned cycle = 0; cycle < 20000; ++cycle) {
        first.tick(4);
        second.tick(4);
        if ((cycle & 15U) == 0) {
            first_endpoint.poll();
            second_endpoint.poll();
        }
        if (!first.serial_port().transfer_active() &&
            !second.serial_port().transfer_active()) {
            break;
        }
    }
    check(first.read8(0xFF01) == 0xC3 && second.read8(0xFF01) == 0x3C &&
              !first.serial_port().transfer_active() &&
              !second.serial_port().transfer_active(),
          "TCP serial endpoint alternates clock ownership after release");

    // A Pokémon probe can rewrite SC while the first TCP response is still
    // in flight. Keep the transport cadence deliberately coarse here; the
    // re-armed transfer must consume that response instead of spinning on an
    // unmatched packet.
    first.write8(0xFF01, 0x96);
    second.write8(0xFF01, 0x69);
    second.write8(0xFF02, 0x80);
    first.write8(0xFF02, 0x81);
    first.tick(512);
    first.write8(0xFF02, 0x81);
    for (unsigned cycle = 0; cycle < 20000; ++cycle) {
        first.tick(4);
        second.tick(4);
        if ((cycle & 15U) == 0) {
            first_endpoint.poll();
            second_endpoint.poll();
        }
        if (!first.serial_port().transfer_active() &&
            !second.serial_port().transfer_active()) {
            break;
        }
    }
    check(first.read8(0xFF01) == 0x69 && second.read8(0xFF01) == 0x96 &&
              !first.serial_port().transfer_active() &&
              !second.serial_port().transfer_active(),
          "TCP endpoint recovers when SC is rewritten before a response");

    // Exercise a short alternating payload, which is the pattern used by
    // Pokémon's trade/battle data exchange after the initial probe. The
    // owner changes every byte and the network is still polled only once per
    // 64 CPU cycles.
    for (unsigned byte = 0; byte < 12; ++byte) {
        // The previous owner's completion callback queues clock_release on
        // its TCP channel. Give both endpoints a normal idle polling window
        // before arming the next byte; otherwise a fast runner can have both
        // guests observe the old peer_clock_busy state and become external
        // receivers, leaving the alternating transfer stalled.
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            first_endpoint.poll();
            second_endpoint.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto host_owns_clock = (byte & 1U) == 0;
        const auto first_value = static_cast<std::uint8_t>(0x30U + byte);
        const auto second_value = static_cast<std::uint8_t>(0xC0U + byte);
        first.write8(0xFF01, first_value);
        second.write8(0xFF01, second_value);
        if (host_owns_clock) {
            second.write8(0xFF02, 0x80);
            first.write8(0xFF02, 0x81);
        } else {
            first.write8(0xFF02, 0x80);
            second.write8(0xFF02, 0x81);
        }
        for (unsigned cycle = 0; cycle < 20000; ++cycle) {
            first.tick(4);
            second.tick(4);
            if ((cycle & 15U) == 0) {
                first_endpoint.poll();
                second_endpoint.poll();
            }
            if (!first.serial_port().transfer_active() &&
                !second.serial_port().transfer_active()) {
                break;
            }
        }
        check(first.read8(0xFF01) == second_value &&
                  second.read8(0xFF01) == first_value &&
                  !first.serial_port().transfer_active() &&
                  !second.serial_port().transfer_active(),
              "TCP endpoint preserves alternating link payload bytes");
    }
    first_endpoint.detach();
    second_endpoint.detach();
}


} // namespace

int main() {
    test_serial_transfer();
    test_serial_link_cable();
    test_serial_link_interrupt_handshake();
    test_serial_link_interrupt_rearm();
    test_serial_link_asymmetric_scheduling();
    test_link_session_lifecycle();
    test_link_session_timeout_and_retry();
    test_link_session_core_neutral_endpoint();
    test_link_transport_framing();
    test_tcp_link_channel_loopback();
    test_tcp_serial_endpoint_loopback();
    return failures == 0 ? 0 : 1;
}
