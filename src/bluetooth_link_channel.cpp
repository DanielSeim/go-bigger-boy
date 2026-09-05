#include "gameboy/bluetooth_link_channel.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2bth.h>
#include <ws2tcpip.h>
#elif defined(__ANDROID__)
extern "C" bool gbb_android_bluetooth_start_host(const char* uuid) noexcept;
extern "C" bool gbb_android_bluetooth_start_join(const char* address,
                                                  const char* uuid) noexcept;
extern "C" int gbb_android_bluetooth_state() noexcept;
extern "C" void gbb_android_bluetooth_stop() noexcept;
extern "C" bool gbb_android_bluetooth_send(const std::uint8_t* bytes,
                                             std::size_t size) noexcept;
extern "C" std::size_t gbb_android_bluetooth_receive(
    std::uint8_t* bytes, std::size_t capacity) noexcept;
#endif

namespace gameboy {
namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;

bool sockets_ready() noexcept {
    static const auto ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

int socket_error() noexcept { return WSAGetLastError(); }
bool would_block(const int error) noexcept {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
void close_socket(const Socket socket) noexcept { closesocket(socket); }
bool set_nonblocking(const Socket socket) noexcept {
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
}
Socket as_socket(const std::intptr_t value) noexcept {
    return static_cast<Socket>(value);
}
std::intptr_t as_handle(const Socket value) noexcept {
    return static_cast<std::intptr_t>(value);
}

bool parse_hex(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty() || text.size() > 16) return false;
    value = 0;
    for (const auto c : text) {
        value <<= 4;
        if (c >= '0' && c <= '9') value |= static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint64_t>(c - 'A' + 10);
        else return false;
    }
    return true;
}

bool parse_address(const std::string& text, BTH_ADDR& address) noexcept {
    std::string compact;
    compact.reserve(text.size());
    for (const auto c : text) {
        if (c == ':' || c == '-' || c == ' ') continue;
        compact.push_back(c);
    }
    std::uint64_t value = 0;
    if (compact.size() != 12 || !parse_hex(compact, value)) return false;
    address = static_cast<BTH_ADDR>(value);
    return true;
}

bool parse_uuid(const std::string& text, GUID& uuid) noexcept {
    std::string compact;
    compact.reserve(text.size());
    for (const auto c : text) if (c != '-') compact.push_back(c);
    if (compact.size() != 32) return false;
    std::uint64_t value = 0;
    if (!parse_hex(std::string_view(compact).substr(0, 8), value)) return false;
    uuid.Data1 = static_cast<unsigned long>(value);
    if (!parse_hex(std::string_view(compact).substr(8, 4), value)) return false;
    uuid.Data2 = static_cast<unsigned short>(value);
    if (!parse_hex(std::string_view(compact).substr(12, 4), value)) return false;
    uuid.Data3 = static_cast<unsigned short>(value);
    for (unsigned i = 0; i < 8; ++i) {
        if (!parse_hex(std::string_view(compact).substr(16 + i * 2, 2), value)) return false;
        uuid.Data4[i] = static_cast<unsigned char>(value);
    }
    return true;
}

bool register_service(const GUID& uuid, const Socket socket,
                      const bool unregister) noexcept {
    SOCKADDR_BTH local{};
    int length = static_cast<int>(sizeof(local));
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) != 0)
        return false;
    static constexpr wchar_t service_name[] = L"Go Bigger Boy Link";
    CSADDR_INFO address{};
    address.LocalAddr.lpSockaddr = reinterpret_cast<sockaddr*>(&local);
    address.LocalAddr.iSockaddrLength = length;
    WSAQUERYSETW query{};
    query.dwSize = sizeof(query);
    query.lpszServiceInstanceName = const_cast<wchar_t*>(service_name);
    query.lpServiceClassId = const_cast<GUID*>(&uuid);
    query.dwNameSpace = NS_BTH;
    query.dwNumberOfCsAddrs = 1;
    query.lpcsaBuffer = &address;
    return WSASetServiceW(&query,
                          unregister ? RNRSERVICE_DEREGISTER : RNRSERVICE_REGISTER,
                          0) == 0;
}
#endif

} // namespace

BluetoothLinkChannel::~BluetoothLinkChannel() { close(); }

bool BluetoothLinkChannel::listen(const std::string& service_uuid) noexcept {
#if defined(_WIN32)
    close();
    if (!sockets_ready()) return false;
    GUID uuid{};
    if (!parse_uuid(service_uuid, uuid)) {
        state_ = State::failed;
        return false;
    }
    const auto socket = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (socket == invalid_socket || !set_nonblocking(socket)) {
        if (socket != invalid_socket) close_socket(socket);
        state_ = State::failed;
        return false;
    }
    SOCKADDR_BTH address{};
    address.addressFamily = AF_BTH;
    address.btAddr = 0;
    address.serviceClassId = GUID_NULL;
    address.port = BT_PORT_ANY;
    const auto valid = ::bind(socket, reinterpret_cast<sockaddr*>(&address),
                              static_cast<int>(sizeof(address))) == 0 &&
                       ::listen(socket, 1) == 0;
    if (!valid || !register_service(uuid, socket, false)) {
        close_socket(socket);
        state_ = State::failed;
        return false;
    }
    listener_ = as_handle(socket);
    service_uuid_ = service_uuid;
    state_ = State::listening;
    return true;
#elif defined(__ANDROID__)
    close();
    if (service_uuid.empty() ||
        !gbb_android_bluetooth_start_host(service_uuid.c_str())) {
        state_ = State::failed;
        return false;
    }
    service_uuid_ = service_uuid;
    state_ = State::listening;
    return true;
#else
    static_cast<void>(service_uuid);
    state_ = State::failed;
    return false;
#endif
}

bool BluetoothLinkChannel::connect(const std::string& address_text,
                                   const std::string& service_uuid) noexcept {
#if defined(_WIN32)
    close();
    if (!sockets_ready()) return false;
    GUID uuid{};
    BTH_ADDR address_value{};
    if (!parse_uuid(service_uuid, uuid) ||
        !parse_address(address_text, address_value)) {
        state_ = State::failed;
        return false;
    }
    const auto socket = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (socket == invalid_socket || !set_nonblocking(socket)) {
        if (socket != invalid_socket) close_socket(socket);
        state_ = State::failed;
        return false;
    }
    SOCKADDR_BTH address{};
    address.addressFamily = AF_BTH;
    address.btAddr = address_value;
    address.serviceClassId = uuid;
    address.port = 0;
    const auto result = ::connect(socket, reinterpret_cast<sockaddr*>(&address),
                                  static_cast<int>(sizeof(address)));
    const auto error = result == 0 ? 0 : socket_error();
    if (result != 0 && !would_block(error)) {
        close_socket(socket);
        state_ = State::failed;
        return false;
    }
    peer_ = as_handle(socket);
    state_ = result == 0 ? State::connected : State::connecting;
    return true;
#elif defined(__ANDROID__)
    close();
    if (address_text.empty() || service_uuid.empty() ||
        !gbb_android_bluetooth_start_join(address_text.c_str(),
                                          service_uuid.c_str())) {
        state_ = State::failed;
        return false;
    }
    service_uuid_ = service_uuid;
    state_ = State::connecting;
    return true;
#else
    static_cast<void>(address_text);
    static_cast<void>(service_uuid);
    state_ = State::failed;
    return false;
#endif
}

void BluetoothLinkChannel::poll() noexcept {
#if defined(_WIN32)
    if (state_ == State::listening && listener_ != -1) {
        const auto accepted = accept(as_socket(listener_), nullptr, nullptr);
        if (accepted != invalid_socket) {
            if (!set_nonblocking(accepted)) close_socket(accepted);
            else {
                peer_ = as_handle(accepted);
                state_ = State::connected;
            }
        }
    }
    if (state_ == State::connecting && peer_ != -1) {
        int error = 0;
        int length = static_cast<int>(sizeof(error));
        if (getsockopt(as_socket(peer_), SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&error), &length) == 0) {
            if (error == 0) state_ = State::connected;
            else if (!would_block(error)) fail();
        }
    }
    if (state_ != State::connected || peer_ == -1) return;
    flush_send_queue();
    receive_available();
#elif defined(__ANDROID__)
    switch (gbb_android_bluetooth_state()) {
    case 1: state_ = State::listening; break;
    case 2: state_ = State::connecting; break;
    case 3: state_ = State::connected; break;
    case 4: state_ = State::failed; break;
    default: state_ = State::disconnected; break;
    }
    if (state_ != State::connected) return;
    flush_send_queue();
    receive_available();
#endif
}

void BluetoothLinkChannel::close() noexcept {
#if defined(_WIN32)
    if (listener_ != -1) {
        if (!service_uuid_.empty()) {
            GUID uuid{};
            if (parse_uuid(service_uuid_, uuid))
                static_cast<void>(register_service(uuid, as_socket(listener_), true));
        }
        close_socket(as_socket(listener_));
    }
    if (peer_ != -1) close_socket(as_socket(peer_));
#elif defined(__ANDROID__)
    gbb_android_bluetooth_stop();
#endif
    listener_ = -1;
    peer_ = -1;
    service_uuid_.clear();
    send_buffer_.clear();
    send_offset_ = 0;
    receive_buffer_.clear();
    packets_.clear();
    malformed_packets_ = 0;
    state_ = State::disconnected;
}

bool BluetoothLinkChannel::send(const LinkPacket& packet) noexcept {
#if defined(_WIN32)
    if (state_ != State::connected) return false;
    const auto wire = LinkPacketCodec::encode(packet);
    send_buffer_.insert(send_buffer_.end(), wire.begin(), wire.end());
    flush_send_queue();
    return true;
#elif defined(__ANDROID__)
    if (state_ != State::connected) return false;
    const auto wire = LinkPacketCodec::encode(packet);
    return gbb_android_bluetooth_send(wire.data(), wire.size());
#else
    static_cast<void>(packet);
    return false;
#endif
}

std::optional<LinkPacket> BluetoothLinkChannel::receive() noexcept {
    if (packets_.empty()) return std::nullopt;
    auto packet = packets_.front();
    packets_.pop_front();
    return packet;
}

void BluetoothLinkChannel::flush_send_queue() noexcept {
#if defined(_WIN32)
    while (send_offset_ < send_buffer_.size() && peer_ != -1) {
        const auto* data = send_buffer_.data() + send_offset_;
        const auto remaining = send_buffer_.size() - send_offset_;
        const auto count = ::send(as_socket(peer_), reinterpret_cast<const char*>(data),
                                  static_cast<int>(remaining), 0);
        if (count > 0) send_offset_ += static_cast<std::size_t>(count);
        else if (count == 0 || !would_block(socket_error())) { fail(); break; }
        else break;
    }
    if (send_offset_ == send_buffer_.size()) {
        send_buffer_.clear();
        send_offset_ = 0;
    } else if (send_offset_ > 4096) {
        send_buffer_.erase(send_buffer_.begin(),
                           send_buffer_.begin() + static_cast<std::ptrdiff_t>(send_offset_));
        send_offset_ = 0;
    }
#endif
}

void BluetoothLinkChannel::receive_available() noexcept {
#if defined(_WIN32)
    std::array<std::uint8_t, 1024> buffer{};
    while (peer_ != -1) {
        const auto count = ::recv(as_socket(peer_), reinterpret_cast<char*>(buffer.data()),
                                  static_cast<int>(buffer.size()), 0);
        if (count > 0) {
            receive_buffer_.insert(receive_buffer_.end(), buffer.begin(), buffer.begin() + count);
            while (receive_buffer_.size() >= LinkPacketCodec::wire_size) {
                const auto packet = LinkPacketCodec::decode(receive_buffer_.data(),
                                                             LinkPacketCodec::wire_size);
                receive_buffer_.erase(receive_buffer_.begin(),
                                      receive_buffer_.begin() + static_cast<std::ptrdiff_t>(LinkPacketCodec::wire_size));
                if (packet) packets_.push_back(*packet);
                else ++malformed_packets_;
            }
        } else {
            if (count == 0 || !would_block(socket_error())) fail();
            break;
        }
    }
#elif defined(__ANDROID__)
    std::array<std::uint8_t, LinkPacketCodec::wire_size> packet{};
    while (gbb_android_bluetooth_receive(packet.data(), packet.size()) != 0) {
        const auto decoded = LinkPacketCodec::decode(packet.data(), packet.size());
        if (decoded) packets_.push_back(*decoded);
        else ++malformed_packets_;
    }
#endif
}

void BluetoothLinkChannel::fail() noexcept {
#if defined(_WIN32)
    if (peer_ != -1) close_socket(as_socket(peer_));
#elif defined(__ANDROID__)
    gbb_android_bluetooth_stop();
#endif
    peer_ = -1;
    send_buffer_.clear();
    send_offset_ = 0;
    receive_buffer_.clear();
    packets_.clear();
    state_ = State::failed;
}

} // namespace gameboy
