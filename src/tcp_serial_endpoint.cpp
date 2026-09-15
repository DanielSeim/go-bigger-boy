#include "gameboy/tcp_serial_endpoint.hpp"

#include "gbb/log.hpp"

#include <atomic>
#include <chrono>

namespace gameboy {
namespace {

std::atomic<std::uint64_t> next_diagnostic_session{1};

std::uint64_t next_packet_session() noexcept {
    static std::atomic<std::uint64_t> counter{1};
    const auto serial = counter.fetch_add(1, std::memory_order_relaxed);
    const auto clock = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto value = clock ^ (serial + UINT64_C(0x9E3779B97F4A7C15));
    return value == 0 ? serial : value;
}

} // namespace

bool LinkSerialEndpoint::send_packet(LinkPacket packet) noexcept {
    if (channel_ == nullptr || session_id_ == 0) return false;
    packet.session_id = session_id_;
    return channel_->send(packet);
}

std::uint32_t LinkSerialEndpoint::state_digest() const noexcept {
    // FNV-1a over the canonical session pair and compatibility identity. The
    // ordering makes both sides calculate the same value even though each
    // endpoint has a different local session token.
    const auto peer_session = peer_session_id_.value_or(0);
    const auto first = session_id_ < peer_session ? session_id_ : peer_session;
    const auto second = session_id_ < peer_session ? peer_session : session_id_;
    std::uint32_t digest = 2166136261U;
    const auto mix = [&digest](const std::uint8_t byte) {
        digest ^= byte;
        digest *= 16777619U;
    };
    for (const auto value : {first, second, compatibility_id_}) {
        for (unsigned byte = 0; byte < 8; ++byte)
            mix(static_cast<std::uint8_t>(value >> (byte * 8U)));
    }
    return digest;
}

bool LinkSerialEndpoint::is_next_sequence(const std::uint32_t previous,
                                           const std::uint32_t next) noexcept {
    return next == previous + 1U;
}

void LinkSerialEndpoint::reset_transfer_state() noexcept {
    pending_sequence_.reset();
    pending_type_.reset();
    pending_packet_.reset();
    pending_retry_polls_ = 0;
    pending_retry_count_ = 0;
    response_.reset();
    byte_response_.reset();
    byte_bits_consumed_ = 0;
    deferred_request_.reset();
    last_peer_request_sequence_.reset();
    last_peer_request_response_.reset();
    request_backoff_ = 0;
    deferred_request_polls_ = 0;
    peer_clock_busy_ = false;
    peer_byte_released_ = false;
    peer_request_seen_ = false;
}

void LinkSerialEndpoint::protocol_fault(const char* message) noexcept {
    ++protocol_errors_;
    gbb::Logger::instance().write(gbb::LogLevel::warning,
                                  gbb::LogCategory::link, message,
                                  {diagnostic_session_, protocol_errors_, 0});
    if (channel_ != nullptr && connected()) channel_->close();
    if (port_ != nullptr) port_->reset_link();
}

void LinkSerialEndpoint::attach(SerialPort& port,
                                LinkPacketChannel& channel,
                                const std::uint64_t link_compatibility_id,
                                const LinkCompatibilityProfile compatibility_profile) noexcept {
    detach();
    port_ = &port;
    channel_ = &channel;
    session_id_ = next_packet_session();
    peer_session_id_.reset();
    peer_state_digest_.reset();
    state_digest_sent_ = false;
    state_digest_valid_ = false;
    next_sequence_ = 0;
    next_control_sequence_ = 0;
    pending_sequence_.reset();
    pending_type_.reset();
    pending_packet_.reset();
    pending_retry_polls_ = 0;
    pending_retry_count_ = 0;
    response_.reset();
    byte_response_.reset();
    byte_bits_consumed_ = 0;
    deferred_request_.reset();
    reset_packet_.reset();
    reset_retry_polls_ = 0;
    reset_retry_count_ = 0;
    last_peer_request_sequence_.reset();
    last_peer_request_response_.reset();
    request_backoff_ = 0;
    deferred_request_polls_ = 0;
    hello_sent_ = false;
    peer_hello_seen_ = false;
    peer_compatible_ = true;
    hello_parts_sent_ = 0;
    hello_parts_received_ = 0;
    compatibility_id_ = link_compatibility_id;
    peer_compatibility_id_ = 0;
    compatibility_profile_ = compatibility_profile;
    peer_compatibility_profile_ = {};
    peer_profile_seen_ = false;
    profile_wait_polls_ = 0;
    peer_request_seen_ = false;
    peer_byte_released_ = false;
    peer_byte_transfer_ = false;
    peer_clock_busy_ = false;
    reset_sequence_.reset();
    reset_waiting_for_ack_ = false;
    reset_ack_wait_polls_ = 0;
    const auto now = std::chrono::steady_clock::now();
    last_peer_activity_ = now;
    last_heartbeat_sent_ = now;
    requests_sent_ = 0;
    requests_received_ = 0;
    responses_sent_ = 0;
    responses_received_ = 0;
    denials_sent_ = 0;
    denials_received_ = 0;
    responses_unmatched_ = 0;
    byte_packets_sent_ = 0;
    byte_packets_received_ = 0;
    stale_session_packets_ = 0;
    out_of_order_requests_ = 0;
    duplicate_requests_ = 0;
    protocol_errors_ = 0;
    heartbeats_sent_ = 0;
    heartbeats_received_ = 0;
    heartbeat_timeouts_ = 0;
    reset_requests_sent_ = 0;
    reset_acknowledgements_ = 0;
    request_retries_ = 0;
    reset_retries_ = 0;
    state_digest_mismatches_ = 0;
    diagnostic_session_ =
        next_diagnostic_session.fetch_add(1, std::memory_order_relaxed);
    port_->set_endpoint(this);
    gbb::Logger::instance().write(gbb::LogLevel::info,
                                  gbb::LogCategory::link,
                                  "packet serial endpoint attached",
                                  {diagnostic_session_, 0, 0});
}

void LinkSerialEndpoint::detach() noexcept {
    const auto was_attached = port_ != nullptr || channel_ != nullptr;
    const auto diagnostic_session = diagnostic_session_;
    // A frontend may stop a remote session while the guest is waiting for a
    // network response. Reset before removing the endpoint, otherwise the
    // emulated SC transfer remains active forever with no cable to complete
    // it.
    if (port_ != nullptr) port_->reset_link();
    if (port_ != nullptr) port_->set_endpoint(nullptr);
    port_ = nullptr;
    channel_ = nullptr;
    session_id_ = 0;
    peer_session_id_.reset();
    peer_state_digest_.reset();
    state_digest_sent_ = false;
    state_digest_valid_ = false;
    next_control_sequence_ = 0;
    pending_sequence_.reset();
    pending_type_.reset();
    pending_packet_.reset();
    pending_retry_polls_ = 0;
    pending_retry_count_ = 0;
    response_.reset();
    byte_response_.reset();
    byte_bits_consumed_ = 0;
    deferred_request_.reset();
    reset_packet_.reset();
    reset_retry_polls_ = 0;
    reset_retry_count_ = 0;
    last_peer_request_sequence_.reset();
    last_peer_request_response_.reset();
    request_backoff_ = 0;
    deferred_request_polls_ = 0;
    hello_sent_ = false;
    peer_hello_seen_ = false;
    peer_compatible_ = true;
    hello_parts_sent_ = 0;
    hello_parts_received_ = 0;
    compatibility_id_ = 0;
    peer_compatibility_id_ = 0;
    compatibility_profile_ = {};
    peer_compatibility_profile_ = {};
    peer_profile_seen_ = false;
    profile_wait_polls_ = 0;
    peer_request_seen_ = false;
    peer_byte_released_ = false;
    peer_byte_transfer_ = false;
    peer_clock_busy_ = false;
    reset_sequence_.reset();
    reset_waiting_for_ack_ = false;
    reset_ack_wait_polls_ = 0;
    last_peer_activity_ = {};
    last_heartbeat_sent_ = {};
    byte_packets_sent_ = 0;
    byte_packets_received_ = 0;
    stale_session_packets_ = 0;
    out_of_order_requests_ = 0;
    duplicate_requests_ = 0;
    protocol_errors_ = 0;
    heartbeats_sent_ = 0;
    heartbeats_received_ = 0;
    heartbeat_timeouts_ = 0;
    reset_requests_sent_ = 0;
    reset_acknowledgements_ = 0;
    request_retries_ = 0;
    reset_retries_ = 0;
    state_digest_mismatches_ = 0;
    diagnostic_session_ = 0;
    if (was_attached) {
        gbb::Logger::instance().write(gbb::LogLevel::info,
                                      gbb::LogCategory::link,
                                      "packet serial endpoint detached",
                                      {diagnostic_session, 0, 0});
    }
}

bool LinkSerialEndpoint::connected() const noexcept {
    return channel_ != nullptr &&
           channel_->state() == LinkPacketChannel::State::connected;
}

void LinkSerialEndpoint::prepare_bit(const bool outgoing) noexcept {
    if (pending_sequence_.has_value() || !connected()) return;
    // The transport connection can come up before the peer has attached its serial
    // endpoint. Hold the first edge until the transport handshake is complete;
    // the serial port will retain its phase at this boundary.
    if (!peer_ready_for_link()) return;
    // Keep the Pokémon join side passive until the host has initiated its
    // first request. This mirrors the physical Cable Club sequence and avoids
    // the late entrant being treated as a competing host.
    if (!arbitration_priority_ &&
        (peer_clock_busy_ || !peer_request_seen_)) {
        // The join side may arm its next internal transfer before the host's
        // clock-release packet arrives. Keep the emulated SC bit asserted and
        // hold the first edge until ownership is released instead of
        // permanently demoting the guest to an external receiver.
        return;
    }
    if (request_backoff_ != 0) {
        --request_backoff_;
        return;
    }
    const auto sequence = next_sequence_++;
    const auto use_byte_transfer = peer_byte_transfer_ && peer_hello_seen_ &&
                                   byte_transfer_allowed() && port_ != nullptr &&
                                   port_->bits_shifted() == 0;
    const LinkPacket packet{
        use_byte_transfer ? LinkPacketType::byte : LinkPacketType::bit,
        sequence,
        use_byte_transfer ? port_->read_data()
                          : static_cast<std::uint8_t>(outgoing ? 1U : 0U),
        request_flag};
    if (send_packet(packet)) {
        pending_sequence_ = sequence;
        pending_type_ = packet.type;
        pending_packet_ = packet;
        pending_retry_polls_ = 0;
        pending_retry_count_ = 0;
        ++requests_sent_;
        if (use_byte_transfer) ++byte_packets_sent_;
    }
}

bool LinkSerialEndpoint::exchange_bit(const bool /*outgoing*/) noexcept {
    if (byte_response_.has_value()) {
        const auto incoming = static_cast<bool>(
            (*byte_response_ >> (7U - byte_bits_consumed_)) & 0x01U);
        ++byte_bits_consumed_;
        if (byte_bits_consumed_ == 8) {
            byte_response_.reset();
            byte_bits_consumed_ = 0;
            pending_sequence_.reset();
            pending_type_.reset();
            pending_packet_.reset();
            pending_retry_polls_ = 0;
            pending_retry_count_ = 0;
        }
        return incoming;
    }
    if (!response_.has_value()) return true;
    const auto incoming = *response_;
    response_.reset();
    pending_sequence_.reset();
    pending_type_.reset();
    pending_packet_.reset();
    pending_retry_polls_ = 0;
    pending_retry_count_ = 0;
    return incoming;
}

bool LinkSerialEndpoint::request_internal_clock(
    SerialPort& /*port*/) noexcept {
    // Permit only one clock owner at a time. The host wins the initial race;
    // after it releases a completed byte, Pokémon may legitimately let the
    // join side clock the next exchange. If the join side arms while the
    // current host byte is still in flight, accept the arm and let
    // prepare_bit() hold the first edge until release rather than demoting
    // the guest to an external receiver.
    if (!connected()) return false;
    if (!arbitration_priority_ && !peer_request_seen_) {
        return false;
    }
    return true;
}

void LinkSerialEndpoint::release_internal_clock(SerialPort& port) noexcept {
    // A guest can rewrite SC while a remote bit is still outstanding (as
    // Pokémon does while probing the Cable Club). Keep the request alive: a
    // TCP response may already be in flight, and dropping it turns every
    // re-arm into an unmatched response that can never advance the byte.
    // reset_link() calls cancel_internal_clock() explicitly when a session is
    // really being abandoned.
    const auto response_ready = response_.has_value() ||
                                byte_response_.has_value();
    if (!response_ready && !port.transfer_active()) {
        pending_sequence_.reset();
        pending_type_.reset();
        pending_packet_.reset();
        pending_retry_polls_ = 0;
        pending_retry_count_ = 0;
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
    static_cast<void>(send_packet(release));
}

void LinkSerialEndpoint::cancel_internal_clock(SerialPort& /*port*/) noexcept {
    // A reset can follow a bit request that is already queued in the peer's
    // socket. Send an ordered reset marker so the peer drops any deferred
    // request from the abandoned transfer before the next guest arms SC.
    if (channel_ != nullptr && connected()) {
        const auto sequence = next_control_sequence_++;
        const LinkPacket reset{LinkPacketType::clock_release, sequence, 0,
                               reset_flag};
        reset_sequence_ = sequence;
        reset_packet_ = reset;
        reset_waiting_for_ack_ = send_packet(reset);
        reset_ack_wait_polls_ = 0;
        reset_retry_polls_ = 0;
        reset_retry_count_ = 0;
        if (reset_waiting_for_ack_) ++reset_requests_sent_;
        else reset_packet_.reset();
    }
    reset_transfer_state();
}

void LinkSerialEndpoint::poll() noexcept {
    if (channel_ == nullptr) return;
    channel_->poll();
    if (!connected()) {
        // A socket can fail after the guest has armed its internal clock. Do
        // the same local cleanup as an explicit retry so the game can return
        // to its own timeout/recovery path instead of freezing at SC=80.
        const auto state = channel_->state();
        if ((state == LinkPacketChannel::State::failed ||
             state == LinkPacketChannel::State::disconnected) &&
            port_ != nullptr) {
            port_->reset_link();
        }
        return;
    }
    if (reset_waiting_for_ack_ &&
        ++reset_ack_wait_polls_ >= reset_ack_wait_poll_limit) {
        protocol_fault("link reset acknowledgement timed out");
        return;
    }
    if (compatibility_profile_.known() && peer_hello_seen_ &&
        !peer_profile_seen_ && profile_wait_polls_ < profile_wait_limit) {
        ++profile_wait_polls_;
    }
    if (!hello_sent_) {
        const auto part = hello_parts_sent_;
        LinkPacket hello{LinkPacketType::hello, part,
                         static_cast<std::uint8_t>(
                             arbitration_priority_ ? 1U : 0U),
                         // Sequence zero has no compatibility payload and
                         // carries capability bits instead. This remains
                         // harmless to older endpoints, which ignore flags
                         // on the arbitration hello.
                         byte_transfer_capability};
        if (compatibility_id_ != 0 && part != 0 && part < 5) {
            const auto shift = static_cast<unsigned>((part - 1U) * 16U);
            hello.value = static_cast<std::uint8_t>(compatibility_id_ >> shift);
            hello.flags = static_cast<std::uint8_t>(compatibility_id_ >> (shift + 8U));
        } else if (compatibility_profile_.known() && part == 5) {
            // Optional profile extension. Legacy peers ignore sequence 5,
            // while current peers can distinguish Gen I, Gen II, and Time
            // Capsule-compatible sessions without changing the packet frame.
            hello.value = static_cast<std::uint8_t>(
                static_cast<std::uint8_t>(compatibility_profile_.generation) |
                (static_cast<std::uint8_t>(compatibility_profile_.region) << 4U));
            hello.flags = static_cast<std::uint8_t>(
                (compatibility_profile_.modes & 0x0fU) |
                ((compatibility_profile_.version & 0x0fU) << 4U));
        }
        if (send_packet(hello)) {
            ++hello_parts_sent_;
            hello_sent_ = compatibility_id_ == 0
                              ? hello_parts_sent_ >= 1
                              : hello_parts_sent_ >=
                                    (compatibility_profile_.known() ? 6 : 5);
            gbb::Logger::instance().write(
                gbb::LogLevel::debug, gbb::LogCategory::link,
                hello_sent_ ? "link hello sent" : "link hello part sent",
                {diagnostic_session_, part, compatibility_id_});
        }
    }

    if (peer_hello_seen_ && peer_session_id_.has_value() &&
        !state_digest_sent_) {
        const LinkPacket digest{LinkPacketType::state_digest, state_digest(),
                                0, 0};
        if (send_packet(digest)) state_digest_sent_ = true;
    }

    const auto service_request = [this](const LinkPacket& packet) {
        const auto byte_request = packet.type == LinkPacketType::byte;
        if (port_ == nullptr || !port_->transfer_active()) {
            // Keep the request at the cable boundary until the guest arms
            // SC. Completing it as "not ready" loses the first byte when the
            // two emulators reach the Cable Club a few frames apart.
            if (!deferred_request_.has_value()) deferred_request_polls_ = 0;
            deferred_request_ = packet;
            return;
        }
        if (port_->internal_clock()) {
            if (arbitration_priority_) {
                const LinkPacket denied{packet.type,
                                        packet.sequence, 1,
                                        static_cast<std::uint8_t>(
                                            response_flag | denied_flag)};
                if (send_packet(denied)) {
                    ++responses_sent_;
                    ++denials_sent_;
                    last_peer_request_response_ = denied;
                }
                // A denied request has been fully handled. Do not leave the
                // host blocked behind a stale peer_clock_busy_ flag while
                // the join side backs off and retries.
                peer_clock_busy_ = false;
                return;
            }
            // The lower-priority requester yields its internal clock and
            // remains an external receiver for the winning peer.
            port_->write_control(static_cast<std::uint8_t>(
                port_->read_control() & ~0x01U));
            pending_sequence_.reset();
            pending_type_.reset();
            response_.reset();
        }
        if (!port_->transfer_active() || port_->internal_clock()) {
            deferred_request_ = packet;
            return;
        }
        std::uint8_t incoming_byte = 0;
        if (byte_request) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                const auto outgoing = port_->clock_external_bit(
                    ((packet.value >> (7U - bit)) & 0x01U) != 0);
                incoming_byte = static_cast<std::uint8_t>(
                    (incoming_byte << 1U) | (outgoing ? 1U : 0U));
            }
        } else {
            const auto outgoing = port_->clock_external_bit(packet.value != 0);
            incoming_byte = static_cast<std::uint8_t>(outgoing ? 1U : 0U);
        }
        const LinkPacket response{packet.type, packet.sequence, incoming_byte,
                                  response_flag};
        if (send_packet(response)) {
            ++responses_sent_;
            last_peer_request_response_ = response;
            if (byte_request) ++byte_packets_sent_;
        }
    };

    while (const auto packet = channel_->receive()) {
        if (packet->session_id == 0) {
            ++stale_session_packets_;
            continue;
        }
        if (!peer_session_id_.has_value()) {
            if (packet->type != LinkPacketType::hello) {
                protocol_fault("link packet arrived before session hello");
                return;
            }
            peer_session_id_ = packet->session_id;
        } else if (packet->session_id != *peer_session_id_) {
            ++stale_session_packets_;
            gbb::Logger::instance().write(
                gbb::LogLevel::warning, gbb::LogCategory::link,
                "link packet belongs to a different session",
                {diagnostic_session_, packet->session_id,
                 *peer_session_id_});
            continue;
        }
        last_peer_activity_ = std::chrono::steady_clock::now();
        if (packet->type == LinkPacketType::hello) {
            if (packet->sequence == 0) {
                peer_byte_transfer_ =
                    (packet->flags & byte_transfer_capability) != 0;
            }
            if (compatibility_id_ == 0) {
                if (!peer_hello_seen_) {
                    gbb::Logger::instance().write(
                        gbb::LogLevel::debug, gbb::LogCategory::link,
                        "link peer hello received",
                        {diagnostic_session_, packet->sequence, 0});
                }
                peer_hello_seen_ = true;
                continue;
            }
            if (packet->sequence == 5) {
                const auto generation = static_cast<LinkGeneration>(
                    packet->value & 0x0fU);
                const auto region = static_cast<LinkRegion>(
                    (packet->value >> 4U) & 0x0fU);
                const auto version = static_cast<std::uint8_t>(
                    (packet->flags >> 4U) & 0x0fU);
                const auto modes = static_cast<std::uint8_t>(packet->flags & 0x0fU);
                peer_compatibility_profile_ = {
                    version,
                    generation, region, modes};
                peer_profile_seen_ = peer_compatibility_profile_.known();
                if (peer_hello_seen_ && compatibility_profile_.known() &&
                    peer_profile_seen_) {
                    peer_compatible_ =
                        peer_compatibility_id_ == compatibility_id_ &&
                        link_profiles_compatible(compatibility_profile_,
                                                 peer_compatibility_profile_);
                }
                continue;
            }
            if (packet->sequence >= 5) continue;
            if (packet->sequence == 0) {
                hello_parts_received_ = static_cast<std::uint8_t>(
                    hello_parts_received_ | 1U);
            } else {
                const auto shift =
                    static_cast<unsigned>((packet->sequence - 1U) * 16U);
                const auto word = static_cast<std::uint64_t>(packet->value) |
                                  (static_cast<std::uint64_t>(packet->flags)
                                   << 8U);
                peer_compatibility_id_ |= word << shift;
                hello_parts_received_ = static_cast<std::uint8_t>(
                    hello_parts_received_ | (1U << packet->sequence));
            }
            if (hello_parts_received_ == 0x1f) {
                peer_hello_seen_ = true;
                peer_compatible_ =
                    peer_compatibility_id_ == compatibility_id_;
                if (compatibility_profile_.known() && peer_profile_seen_) {
                    peer_compatible_ =
                        peer_compatible_ &&
                        link_profiles_compatible(compatibility_profile_,
                                                 peer_compatibility_profile_);
                }
                gbb::Logger::instance().write(
                    peer_compatible_ ? gbb::LogLevel::debug
                                     : gbb::LogLevel::warning,
                    gbb::LogCategory::link,
                    peer_compatible_ ? "link peer hello received"
                                     : "link peer compatibility mismatch",
                    {diagnostic_session_, peer_compatibility_id_,
                     compatibility_id_});
            }
            continue;
        }
        if (packet->type == LinkPacketType::heartbeat) {
            ++heartbeats_received_;
            const LinkPacket acknowledgement{LinkPacketType::acknowledgement,
                                             packet->sequence, 0,
                                             heartbeat_ack_flag};
            static_cast<void>(send_packet(acknowledgement));
            continue;
        }
        if (packet->type == LinkPacketType::state_digest) {
            peer_state_digest_ = packet->sequence;
            if (packet->sequence != state_digest()) {
                ++state_digest_mismatches_;
                protocol_fault("link state digest mismatch");
                return;
            }
            state_digest_valid_ = true;
            continue;
        }
        if (packet->type == LinkPacketType::acknowledgement) {
            if ((packet->flags & heartbeat_ack_flag) != 0)
                ++heartbeats_received_;
            continue;
        }
        if (packet->type == LinkPacketType::clock_release) {
            if ((packet->flags & reset_flag) != 0) {
                if ((packet->flags & reset_ack_flag) != 0) {
                    if (reset_waiting_for_ack_ && reset_sequence_.has_value() &&
                        packet->sequence == *reset_sequence_) {
                        reset_waiting_for_ack_ = false;
                        reset_packet_.reset();
                        reset_retry_polls_ = 0;
                        reset_retry_count_ = 0;
                        reset_ack_wait_polls_ = 0;
                        ++reset_acknowledgements_;
                    }
                    continue;
                }
                // A peer can reset just after this side has started a fresh
                // request. Do not let an older reset marker cancel that
                // newer request; markers carry the sender's next sequence
                // number for this ordering check.
                if (pending_sequence_.has_value() &&
                    packet->sequence < *pending_sequence_) {
                    continue;
                }
                reset_transfer_state();
                const LinkPacket acknowledgement{LinkPacketType::clock_release,
                                                 packet->sequence, 0,
                                                 static_cast<std::uint8_t>(
                                                     reset_flag |
                                                     reset_ack_flag)};
                static_cast<void>(send_packet(acknowledgement));
                continue;
            }
            peer_clock_busy_ = false;
            if (packet->value != 0) peer_byte_released_ = true;
            continue;
        }
        if (packet->type != LinkPacketType::bit &&
            packet->type != LinkPacketType::byte)
            continue;
        if (packet->type == LinkPacketType::byte) ++byte_packets_received_;
        if ((packet->flags & request_flag) != 0) {
            if ((packet->flags & response_flag) != 0) {
                protocol_fault("link packet marked as request and response");
                return;
            }
            if (last_peer_request_sequence_.has_value()) {
                if (packet->sequence == *last_peer_request_sequence_) {
                    ++duplicate_requests_;
                    if (last_peer_request_response_.has_value() &&
                        send_packet(*last_peer_request_response_))
                        ++responses_sent_;
                    continue;
                }
                if (!is_next_sequence(*last_peer_request_sequence_,
                                       packet->sequence)) {
                    ++out_of_order_requests_;
                    protocol_fault("out-of-order link request");
                    return;
                }
            }
            if (deferred_request_.has_value() &&
                packet->sequence != deferred_request_->sequence) {
                ++out_of_order_requests_;
                protocol_fault("multiple link requests are in flight");
                return;
            }
            last_peer_request_sequence_ = packet->sequence;
            last_peer_request_response_.reset();
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
                    "link response did not match a pending request",
                    {diagnostic_session_, 0, 0});
                continue;
            }
            if (!pending_type_.has_value() ||
                packet->type != *pending_type_) {
                protocol_fault("link response type does not match request");
                return;
            }
            if ((packet->flags & denied_flag) != 0) {
                ++denials_received_;
                gbb::Logger::instance().write(
                    gbb::LogLevel::debug, gbb::LogCategory::link,
                    "link request denied; backing off before retry",
                    {diagnostic_session_, 0, 0});
                if (port_ != nullptr && port_->transfer_active() &&
                    port_->internal_clock()) {
                    port_->write_control(static_cast<std::uint8_t>(
                        port_->read_control() & ~0x01U));
                }
                pending_sequence_.reset();
                pending_type_.reset();
                pending_packet_.reset();
                pending_retry_polls_ = 0;
                pending_retry_count_ = 0;
                response_.reset();
                byte_response_.reset();
                byte_bits_consumed_ = 0;
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
                    "link peer is not ready; backing off before retry",
                    {diagnostic_session_, 0, 0});
                pending_sequence_.reset();
                pending_type_.reset();
                pending_packet_.reset();
                pending_retry_polls_ = 0;
                pending_retry_count_ = 0;
                response_.reset();
                request_backoff_ = 64;
            } else {
                if (packet->type == LinkPacketType::byte) {
                    byte_response_ = packet->value;
                    byte_bits_consumed_ = 0;
                } else {
                    response_ = packet->value != 0;
                }
            }
        } else {
            protocol_fault("link data packet has no request or response flag");
            return;
        }
    }
    if (pending_packet_.has_value() && pending_sequence_.has_value() &&
        !response_.has_value() && !byte_response_.has_value() &&
        ++pending_retry_polls_ >= retry_interval_polls) {
        if (pending_retry_count_ >= maximum_request_retries) {
            protocol_fault("serial request acknowledgement timed out");
            return;
        }
        if (send_packet(*pending_packet_)) {
            ++pending_retry_count_;
            ++request_retries_;
            pending_retry_polls_ = 0;
        }
    }
    if (reset_waiting_for_ack_ && reset_packet_ &&
        ++reset_retry_polls_ >= retry_interval_polls) {
        if (reset_retry_count_ >= maximum_request_retries) {
            protocol_fault("serial reset retry limit exceeded");
            return;
        }
        if (send_packet(*reset_packet_)) {
            ++reset_retry_count_;
            ++reset_retries_;
            reset_retry_polls_ = 0;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_peer_activity_ >= heartbeat_timeout) {
        ++heartbeat_timeouts_;
        protocol_fault("link peer heartbeat timed out");
        return;
    }
    if (now - last_heartbeat_sent_ >= heartbeat_interval) {
        const LinkPacket heartbeat{LinkPacketType::heartbeat,
                                   next_control_sequence_++,
                                   0, 0};
        if (send_packet(heartbeat)) {
            last_heartbeat_sent_ = now;
            ++heartbeats_sent_;
        }
    }
    if (deferred_request_.has_value()) {
        if (port_ != nullptr && port_->transfer_active() &&
            !port_->internal_clock()) {
            const auto request = *deferred_request_;
            deferred_request_.reset();
            deferred_request_polls_ = 0;
            service_request(request);
        } else if (port_ == nullptr || !port_->transfer_active()) {
            // A peer that has stopped arming SC must not leave the other
            // endpoint's request pending indefinitely. Return a retryable
            // response after a bounded grace period so the guest's own link
            // timeout/recovery path can run instead of freezing both sides.
            ++deferred_request_polls_;
            if (deferred_request_polls_ >= deferred_request_poll_limit) {
                const auto request = *deferred_request_;
                deferred_request_.reset();
                deferred_request_polls_ = 0;
                gbb::Logger::instance().write(
                    gbb::LogLevel::warning, gbb::LogCategory::link,
                    "deferred link request expired; returning not-ready",
                    {diagnostic_session_, request.sequence,
                     deferred_request_poll_limit});
                const LinkPacket not_ready{request.type, request.sequence, 0,
                                           static_cast<std::uint8_t>(
                                               response_flag | not_ready_flag)};
                if (send_packet(not_ready)) {
                    ++responses_sent_;
                    last_peer_request_response_ = not_ready;
                }
            }
        }
    }
}

} // namespace gameboy
