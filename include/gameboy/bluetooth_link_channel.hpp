#pragma once

#include "gameboy/link_packet_channel.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace gameboy {

// Bluetooth Classic RFCOMM packet channel.  RFCOMM is exposed as a reliable
// byte stream on Android and Windows, so it uses the same LinkPacket framing
// as TCP. Platform setup (pairing, device selection, and SDP) stays outside
// this class; the channel only owns the non-blocking data path.
class BluetoothLinkChannel final : public LinkPacketChannel {
public:
    using State = LinkPacketChannel::State;

    BluetoothLinkChannel() noexcept = default;
    BluetoothLinkChannel(const BluetoothLinkChannel&) = delete;
    BluetoothLinkChannel& operator=(const BluetoothLinkChannel&) = delete;
    ~BluetoothLinkChannel() override;

    // service_uuid is the fixed application UUID shared by both peers.
    // address is a platform Bluetooth address for join, and is ignored when
    // hosting. Android may use a paired-device identifier supplied by Java.
    [[nodiscard]] bool listen(const std::string& service_uuid) noexcept;
    [[nodiscard]] bool connect(const std::string& address,
                               const std::string& service_uuid) noexcept;

    void poll() noexcept override;
    void close() noexcept override;
    [[nodiscard]] bool send(const LinkPacket& packet) noexcept override;
    [[nodiscard]] std::optional<LinkPacket> receive() noexcept override;
    [[nodiscard]] State state() const noexcept override { return state_; }
    [[nodiscard]] std::uint64_t malformed_packets() const noexcept override {
        return malformed_packets_;
    }

private:
    void flush_send_queue() noexcept;
    void receive_available() noexcept;
    void fail() noexcept;

    std::intptr_t listener_{-1};
    std::intptr_t peer_{-1};
    std::string service_uuid_;
    State state_{State::disconnected};
    std::vector<std::uint8_t> send_buffer_;
    std::size_t send_offset_{};
    std::vector<std::uint8_t> receive_buffer_;
    std::deque<LinkPacket> packets_;
    std::uint64_t malformed_packets_{};
};

} // namespace gameboy
