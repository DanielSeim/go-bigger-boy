#include "gameboy/tcp_serial_endpoint.hpp"

#include "gbb/log.hpp"

#include <atomic>

namespace gameboy {
namespace {

std::atomic<std::uint64_t> next_diagnostic_session{1};

} // namespace

void TcpSerialEndpoint::attach(SerialPort& port,
                                TcpLinkChannel& channel) noexcept {
    detach();
    port_ = &port;
    channel_ = &channel;
    next_sequence_ = 0;
    pending_sequence_.reset();
    response_.reset();
    deferred_request_.reset();
    request_backoff_ = 0;
    hello_sent_ = false;
    peer_hello_seen_ = false;
    peer_request_seen_ = false;
    peer_byte_released_ = false;
    peer_clock_busy_ = false;
    requests_sent_ = 0;
    requests_received_ = 0;
    responses_sent_ = 0;
    responses_received_ = 0;
    denials_sent_ = 0;
    denials_received_ = 0;
    responses_unmatched_ = 0;
    diagnostic_session_ =
        next_diagnostic_session.fetch_add(1, std::memory_order_relaxed);
    port_->set_endpoint(this);
    gbb::Logger::instance().write(gbb::LogLevel::info,
                                  gbb::LogCategory::link,
                                  "TCP serial endpoint attached",
                                  {diagnostic_session_, 0, 0});
}

void TcpSerialEndpoint::detach() noexcept {
    const auto was_attached = port_ != nullptr || channel_ != nullptr;
    const auto diagnostic_session = diagnostic_session_;
    if (port_ != nullptr) port_->set_endpoint(nullptr);
    port_ = nullptr;
    channel_ = nullptr;
    pending_sequence_.reset();
    response_.reset();
    deferred_request_.reset();
    request_backoff_ = 0;
    hello_sent_ = false;
    peer_hello_seen_ = false;
    peer_request_seen_ = false;
    peer_byte_released_ = false;
    peer_clock_busy_ = false;
    diagnostic_session_ = 0;
    if (was_attached) {
        gbb::Logger::instance().write(gbb::LogLevel::info,
                                      gbb::LogCategory::link,
                                      "TCP serial endpoint detached",
                                      {diagnostic_session, 0, 0});
    }
}

bool TcpSerialEndpoint::connected() const noexcept {
    return channel_ != nullptr &&
           channel_->state() == TcpLinkChannel::State::connected;
}

void TcpSerialEndpoint::prepare_bit(const bool outgoing) noexcept {
    if (pending_sequence_.has_value() || !connected()) return;
    // The TCP connection can come up before the peer has attached its serial
    // endpoint. Hold the first edge until the transport handshake is complete;
    // the serial port will retain its phase at this boundary.
    if (!peer_ready_for_link()) return;
    // Keep the Pokémon join side passive until the host has initiated its
    // first request. This mirrors the physical Cable Club sequence and avoids
    // the late entrant being treated as a competing host.
    if (!arbitration_priority_ && !peer_request_seen_) return;
    if (request_backoff_ != 0) {
        --request_backoff_;
        return;
    }
    const auto sequence = next_sequence_++;
    const LinkPacket packet{LinkPacketType::bit, sequence,
                            static_cast<std::uint8_t>(outgoing ? 1U : 0U),
                            request_flag};
    if (channel_->send(packet)) {
        pending_sequence_ = sequence;
        ++requests_sent_;
    }
}

bool TcpSerialEndpoint::exchange_bit(const bool /*outgoing*/) noexcept {
    if (!response_.has_value()) return true;
    const auto incoming = *response_;
    response_.reset();
    pending_sequence_.reset();
    return incoming;
}

bool TcpSerialEndpoint::request_internal_clock(
    SerialPort& /*port*/) noexcept {
    // Permit only one clock owner at a time. The host wins the initial race;
    // after it releases a completed byte, Pokémon may legitimately let the
    // join side clock the next exchange.
    if (!connected() || peer_clock_busy_) return false;
    if (!arbitration_priority_ &&
        (!peer_request_seen_ || !peer_byte_released_)) {
        return false;
    }
    return true;
}

void TcpSerialEndpoint::release_internal_clock(SerialPort& port) noexcept {
    // A guest can rewrite SC while a remote bit is still outstanding (as
    // Pokémon does while probing the Cable Club). Keep the request alive: a
    // TCP response may already be in flight, and dropping it turns every
    // re-arm into an unmatched response that can never advance the byte.
    // reset_link() calls cancel_internal_clock() explicitly when a session is
    // really being abandoned.
    const auto response_ready = response_.has_value();
    if (!response_ready && !port.transfer_active()) {
        pending_sequence_.reset();
        request_backoff_ = 0;
    }
    // A response that is already ready belongs to the next edge and must not
    // be advertised as an abort. The peer will receive the normal completed
    // release once that edge finishes.
    if (response_ready && port.transfer_active()) return;
    if (channel_ == nullptr || !connected()) return;
    // A completed byte and an aborted/reprogrammed transfer both release the
    // current owner, but only a completed byte grants the join side permission
    // to become the next clock owner during initial negotiation.
    const LinkPacket release{LinkPacketType::clock_release, 0,
                             static_cast<std::uint8_t>(
                                 port.transfer_active() ? 0U : 1U),
                             0};
    static_cast<void>(channel_->send(release));
}

void TcpSerialEndpoint::cancel_internal_clock(SerialPort& /*port*/) noexcept {
    // A reset can follow a bit request that is already queued in the peer's
    // socket. Send an ordered reset marker so the peer drops any deferred
    // request from the abandoned transfer before the next guest arms SC.
    if (channel_ != nullptr && connected()) {
        const LinkPacket reset{LinkPacketType::clock_release, next_sequence_, 0,
                               reset_flag};
        static_cast<void>(channel_->send(reset));
    }
    pending_sequence_.reset();
    response_.reset();
    deferred_request_.reset();
    request_backoff_ = 0;
    peer_clock_busy_ = false;
}

void TcpSerialEndpoint::poll() noexcept {
    if (channel_ == nullptr) return;
    channel_->poll();
    if (!connected()) return;
    if (!hello_sent_) {
        const LinkPacket hello{LinkPacketType::hello, 0,
                               static_cast<std::uint8_t>(
                                   arbitration_priority_ ? 1U : 0U),
                               0};
        if (channel_->send(hello)) {
            hello_sent_ = true;
            gbb::Logger::instance().write(gbb::LogLevel::debug,
                                          gbb::LogCategory::link,
                                          "TCP link hello sent",
                                          {diagnostic_session_, 0, 0});
        }
    }

    const auto service_request = [this](const LinkPacket& packet) {
        if (port_ == nullptr || !port_->transfer_active()) {
            // Keep the request at the cable boundary until the guest arms
            // SC. Completing it as "not ready" loses the first byte when the
            // two emulators reach the Cable Club a few frames apart.
            deferred_request_ = packet;
            return;
        }
        if (port_->internal_clock()) {
            if (arbitration_priority_) {
                const LinkPacket denied{LinkPacketType::bit,
                                        packet.sequence, 1,
                                        static_cast<std::uint8_t>(
                                            response_flag | denied_flag)};
                if (channel_->send(denied)) {
                    ++responses_sent_;
                    ++denials_sent_;
                }
                return;
            }
            // The lower-priority requester yields its internal clock and
            // remains an external receiver for the winning peer.
            port_->write_control(static_cast<std::uint8_t>(
                port_->read_control() & ~0x01U));
            pending_sequence_.reset();
            response_.reset();
        }
        if (!port_->transfer_active() || port_->internal_clock()) {
            deferred_request_ = packet;
            return;
        }
        const auto outgoing = port_->clock_external_bit(packet.value != 0);
        const LinkPacket response{LinkPacketType::bit, packet.sequence,
                                  static_cast<std::uint8_t>(outgoing ? 1U
                                                                      : 0U),
                                  response_flag};
        if (channel_->send(response)) ++responses_sent_;
    };

    while (const auto packet = channel_->receive()) {
        if (packet->type == LinkPacketType::hello) {
            if (!peer_hello_seen_) {
                gbb::Logger::instance().write(
                    gbb::LogLevel::debug, gbb::LogCategory::link,
                    "TCP link peer hello received",
                    {diagnostic_session_, 0, 0});
            }
            peer_hello_seen_ = true;
            continue;
        }
        if (packet->type == LinkPacketType::clock_release) {
            if ((packet->flags & reset_flag) != 0) {
                // A peer can reset just after this side has started a fresh
                // request. Do not let an older reset marker cancel that
                // newer request; markers carry the sender's next sequence
                // number for this ordering check.
                if (pending_sequence_.has_value() &&
                    packet->sequence < *pending_sequence_) {
                    continue;
                }
                pending_sequence_.reset();
                response_.reset();
                deferred_request_.reset();
                request_backoff_ = 0;
                peer_clock_busy_ = false;
                peer_byte_released_ = false;
                peer_request_seen_ = false;
                continue;
            }
            peer_clock_busy_ = false;
            if (packet->value != 0) peer_byte_released_ = true;
            continue;
        }
        if (packet->type != LinkPacketType::bit) continue;
        if ((packet->flags & request_flag) != 0) {
            ++requests_received_;
            peer_request_seen_ = true;
            peer_clock_busy_ = true;
            service_request(*packet);
        } else if ((packet->flags & response_flag) != 0) {
            ++responses_received_;
            if (!pending_sequence_.has_value() ||
                packet->sequence != *pending_sequence_) {
                // Keep the packet count useful when diagnosing stale or
                // cross-session frames, but never let an unmatched response
                // alter the emulated serial state.
                ++responses_unmatched_;
                gbb::Logger::instance().write(
                    gbb::LogLevel::warning, gbb::LogCategory::link,
                    "TCP link response did not match a pending request",
                    {diagnostic_session_, 0, 0});
                continue;
            }
            if ((packet->flags & denied_flag) != 0) {
                ++denials_received_;
                gbb::Logger::instance().write(
                    gbb::LogLevel::debug, gbb::LogCategory::link,
                    "TCP link request denied; backing off before retry",
                    {diagnostic_session_, 0, 0});
                if (port_ != nullptr && port_->transfer_active() &&
                    port_->internal_clock()) {
                    port_->write_control(static_cast<std::uint8_t>(
                        port_->read_control() & ~0x01U));
                }
                pending_sequence_.reset();
                response_.reset();
                // Let the winning host arm its external receiver before the
                // join side retries. Without this yield, Pokémon can rewrite
                // SC immediately and generate a denial storm that leaves the
                // two Cable Club state machines in different phases.
                request_backoff_ = 128;
            } else if ((packet->flags & not_ready_flag) != 0) {
                // Keep the serial phase at the next edge, but avoid hammering
                // the socket while the guest ISR re-arms its receiver.
                gbb::Logger::instance().write(
                    gbb::LogLevel::debug, gbb::LogCategory::link,
                    "TCP link peer is not ready; backing off before retry",
                    {diagnostic_session_, 0, 0});
                pending_sequence_.reset();
                response_.reset();
                request_backoff_ = 64;
            } else {
                response_ = packet->value != 0;
            }
        }
    }
    if (deferred_request_.has_value() && port_ != nullptr &&
        port_->transfer_active() && !port_->internal_clock()) {
        const auto request = *deferred_request_;
        deferred_request_.reset();
        service_request(request);
    }
}

} // namespace gameboy
