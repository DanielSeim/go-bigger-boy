#include "gameboy/tcp_link_channel.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
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
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
bool sockets_ready() noexcept { return true; }
int socket_error() noexcept { return errno; }
bool would_block(const int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
}
void close_socket(const Socket socket) noexcept { close(socket); }
bool set_nonblocking(const Socket socket) noexcept {
    const auto flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

Socket as_socket(const std::intptr_t value) noexcept {
    return static_cast<Socket>(value);
}

std::intptr_t as_handle(const Socket value) noexcept {
    return static_cast<std::intptr_t>(value);
}

// Serial bit requests are latency-sensitive: the emulated clock waits for
// the peer's response before producing its next edge. Disable Nagle's
// algorithm so tiny request/response frames are sent immediately. This is a
// best-effort tuning knob and must not make a valid link unusable on a
// platform that does not expose the option.
void set_low_latency(const Socket socket) noexcept {
    int enabled = 1;
    static_cast<void>(setsockopt(
        socket, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&enabled), sizeof(enabled)));
}

} // namespace

TcpLinkChannel::~TcpLinkChannel() { close(); }

bool TcpLinkChannel::listen(const std::uint16_t port) noexcept {
    return listen(port, "127.0.0.1");
}

bool TcpLinkChannel::listen(const std::uint16_t port,
                            const std::string& bind_address) noexcept {
    close();
    if (!sockets_ready()) return false;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* result = nullptr;
    const auto service = std::to_string(port);
    const char* host = bind_address.empty() ? nullptr : bind_address.c_str();
    if (getaddrinfo(host, service.c_str(), &hints, &result) != 0 ||
        result == nullptr) {
        state_ = State::failed;
        return false;
    }
    const auto socket = ::socket(result->ai_family, result->ai_socktype,
                                 result->ai_protocol);
    int reuse = 1;
    if (socket != invalid_socket) {
        static_cast<void>(setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                                     reinterpret_cast<const char*>(&reuse),
                                     sizeof(reuse)));
    }
    const auto valid = socket != invalid_socket && set_nonblocking(socket) &&
                       ::bind(socket, result->ai_addr,
                              static_cast<int>(result->ai_addrlen)) == 0 &&
                       ::listen(socket, 1) == 0;
    freeaddrinfo(result);
    if (!valid) {
        if (socket != invalid_socket) close_socket(socket);
        state_ = State::failed;
        return false;
    }
    set_low_latency(socket);
    listener_ = as_handle(socket);
    state_ = State::listening;
    return true;
}

bool TcpLinkChannel::connect(const std::string& host,
                             const std::uint16_t port) noexcept {
    close();
    if (!sockets_ready()) return false;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const auto service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 ||
        result == nullptr) {
        state_ = State::failed;
        return false;
    }
    const auto socket = ::socket(result->ai_family, result->ai_socktype,
                                 result->ai_protocol);
    if (socket == invalid_socket || !set_nonblocking(socket)) {
        if (socket != invalid_socket) close_socket(socket);
        freeaddrinfo(result);
        state_ = State::failed;
        return false;
    }
    set_low_latency(socket);
    const auto connected = ::connect(socket, result->ai_addr,
                                     static_cast<int>(result->ai_addrlen));
    const auto error = connected == 0 ? 0 : socket_error();
    freeaddrinfo(result);
    if (connected != 0 && !would_block(error)) {
        close_socket(socket);
        state_ = State::failed;
        return false;
    }
    peer_ = as_handle(socket);
    state_ = connected == 0 ? State::connected : State::connecting;
    return true;
}

void TcpLinkChannel::poll() noexcept {
    if (state_ == State::listening && listener_ != -1) {
        const auto accepted = accept(as_socket(listener_), nullptr, nullptr);
        if (accepted != invalid_socket) {
            if (!set_nonblocking(accepted)) {
                close_socket(accepted);
            } else {
                set_low_latency(accepted);
                peer_ = as_handle(accepted);
                state_ = State::connected;
            }
        }
    }
    if (state_ == State::connecting && peer_ != -1) {
        int error = 0;
#if defined(_WIN32)
        int length = static_cast<int>(sizeof(error));
#else
        auto length = static_cast<socklen_t>(sizeof(error));
#endif
        if (getsockopt(as_socket(peer_), SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&error), &length) == 0) {
            if (error == 0) {
                state_ = State::connected;
            } else if (!would_block(error)) {
                fail();
            }
        }
    }
    if (state_ != State::connected || peer_ == -1) return;
    flush_send_queue();
    receive_available();
}

void TcpLinkChannel::close() noexcept {
    if (listener_ != -1) close_socket(as_socket(listener_));
    if (peer_ != -1) close_socket(as_socket(peer_));
    listener_ = -1;
    peer_ = -1;
    send_buffer_.clear();
    send_offset_ = 0;
    receive_buffer_.clear();
    packets_.clear();
    malformed_packets_ = 0;
    state_ = State::disconnected;
}

bool TcpLinkChannel::send(const LinkPacket& packet) noexcept {
    if (state_ != State::connected) return false;
    const auto wire = LinkPacketCodec::encode(packet);
    send_buffer_.insert(send_buffer_.end(), wire.begin(), wire.end());
    flush_send_queue();
    return true;
}

std::optional<LinkPacket> TcpLinkChannel::receive() noexcept {
    if (packets_.empty()) return std::nullopt;
    auto packet = packets_.front();
    packets_.pop_front();
    return packet;
}

std::uint16_t TcpLinkChannel::local_port() const noexcept {
    const auto socket = listener_ != -1 ? as_socket(listener_) : invalid_socket;
    if (socket == invalid_socket) return 0;
    sockaddr_in address{};
#if defined(_WIN32)
    int length = static_cast<int>(sizeof(address));
#else
    auto length = static_cast<socklen_t>(sizeof(address));
#endif
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

void TcpLinkChannel::flush_send_queue() noexcept {
    while (send_offset_ < send_buffer_.size() && peer_ != -1) {
        const auto* data = send_buffer_.data() + send_offset_;
        const auto remaining = send_buffer_.size() - send_offset_;
        const auto count = ::send(as_socket(peer_),
                                  reinterpret_cast<const char*>(data),
                                  static_cast<int>(remaining), 0);
        if (count > 0) {
            send_offset_ += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            fail();
        } else if (!would_block(socket_error())) {
            fail();
        }
        break;
    }
    if (send_offset_ == send_buffer_.size()) {
        send_buffer_.clear();
        send_offset_ = 0;
    } else if (send_offset_ > 0 && send_offset_ > 4096) {
        send_buffer_.erase(send_buffer_.begin(),
                           send_buffer_.begin() +
                               static_cast<std::ptrdiff_t>(send_offset_));
        send_offset_ = 0;
    }
}

void TcpLinkChannel::receive_available() noexcept {
    std::array<std::uint8_t, 1024> buffer{};
    while (peer_ != -1) {
        const auto count = ::recv(as_socket(peer_),
                                  reinterpret_cast<char*>(buffer.data()),
                                  static_cast<int>(buffer.size()), 0);
        if (count > 0) {
            receive_buffer_.insert(receive_buffer_.end(), buffer.begin(),
                                    buffer.begin() + count);
            while (receive_buffer_.size() >= LinkPacketCodec::wire_size) {
                const auto packet = LinkPacketCodec::decode(
                    receive_buffer_.data(), LinkPacketCodec::wire_size);
                receive_buffer_.erase(
                    receive_buffer_.begin(),
                    receive_buffer_.begin() +
                        static_cast<std::ptrdiff_t>(LinkPacketCodec::wire_size));
                if (packet) {
                    packets_.push_back(*packet);
                } else {
                    ++malformed_packets_;
                }
            }
            continue;
        }
        if (count == 0) {
            fail();
        } else if (!would_block(socket_error())) {
            fail();
        }
        break;
    }
}

void TcpLinkChannel::fail() noexcept {
    if (peer_ != -1) close_socket(as_socket(peer_));
    peer_ = -1;
    send_buffer_.clear();
    send_offset_ = 0;
    receive_buffer_.clear();
    packets_.clear();
    state_ = State::failed;
}

} // namespace gameboy
