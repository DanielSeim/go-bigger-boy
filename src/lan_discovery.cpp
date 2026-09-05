#include "gameboy/lan_discovery.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <sstream>

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

std::intptr_t as_handle(const Socket socket) noexcept {
    return static_cast<std::intptr_t>(socket);
}
Socket as_socket(const std::intptr_t socket) noexcept {
    return static_cast<Socket>(socket);
}

std::string fingerprint_text(const std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << value;
    return output.str();
}

bool parse_hex(const std::string& text, std::uint64_t& value) noexcept {
    if (text.empty() || text.size() > 16) return false;
    value = 0;
    for (const auto character : text) {
        value <<= 4;
        if (character >= '0' && character <= '9') {
            value |= static_cast<std::uint64_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            value |= static_cast<std::uint64_t>(character - 'a' + 10);
        } else if (character >= 'A' && character <= 'F') {
            value |= static_cast<std::uint64_t>(character - 'A' + 10);
        } else {
            return false;
        }
    }
    return true;
}

std::string sanitize_name(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](const char c) {
                    return c == '\r' || c == '\n';
                }),
                value.end());
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    if (value.empty()) value = "Go Bigger Boy";
    if (value.size() > 48) value.resize(48);
    return value;
}

} // namespace

LanDiscovery::~LanDiscovery() { stop(); }

bool LanDiscovery::start_host(const std::uint16_t tcp_port,
                              const std::uint64_t rom_fingerprint,
                              const std::string& name) noexcept {
    stop();
    if (!sockets_ready()) return false;
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == invalid_socket || !set_nonblocking(socket)) {
        if (socket != invalid_socket) close_socket(socket);
        return false;
    }
    int reuse = 1;
    static_cast<void>(setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                                 reinterpret_cast<const char*>(&reuse),
                                 sizeof(reuse)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(discovery_port);
    if (bind(socket, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0) {
        close_socket(socket);
        return false;
    }
    socket_ = as_handle(socket);
    mode_ = Mode::host;
    tcp_port_ = tcp_port;
    rom_fingerprint_ = rom_fingerprint;
    name_ = sanitize_name(name);
    return true;
}

bool LanDiscovery::start_scan(const std::uint64_t rom_fingerprint) noexcept {
    stop();
    if (!sockets_ready()) return false;
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == invalid_socket || !set_nonblocking(socket)) {
        if (socket != invalid_socket) close_socket(socket);
        return false;
    }
    int broadcast = 1;
    static_cast<void>(setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
                                 reinterpret_cast<const char*>(&broadcast),
                                 sizeof(broadcast)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = 0;
    if (bind(socket, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0) {
        close_socket(socket);
        return false;
    }
    socket_ = as_handle(socket);
    mode_ = Mode::scan;
    rom_fingerprint_ = rom_fingerprint;
    peers_.clear();
    const auto message = std::string("GBB-DISCOVERY/1 Q ") +
                         fingerprint_text(rom_fingerprint_);
    // A broadcast can be rejected by a host firewall even when ordinary
    // unicast UDP is available. Keep the scanner alive so the loopback and
    // any later interface-specific responses can still be consumed.
    const auto broadcast_sent =
        send_message(message, "255.255.255.255", discovery_port);
    // Loopback makes discovery testable and covers hosts where broadcast is
    // filtered by the local firewall; it does not replace the LAN broadcast.
    const auto loopback_sent =
        send_message(message, "127.0.0.1", discovery_port);
    if (!broadcast_sent && !loopback_sent) {
        stop();
        return false;
    }
    return true;
}

void LanDiscovery::poll() noexcept {
    if (socket_ == -1) return;
    receive_available();
}

void LanDiscovery::stop() noexcept {
    if (socket_ != -1) close_socket(as_socket(socket_));
    socket_ = -1;
    peers_.clear();
    name_.clear();
    tcp_port_ = 0;
    rom_fingerprint_ = 0;
}

std::vector<LanPeer> LanDiscovery::take_peers() {
    auto peers = std::move(peers_);
    peers_.clear();
    return peers;
}

bool LanDiscovery::send_message(const std::string& message,
                                const char* address,
                                const std::uint16_t port) noexcept {
    if (socket_ == -1 || address == nullptr) return false;
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &destination.sin_addr) != 1) return false;
    const auto count = sendto(as_socket(socket_), message.data(),
                              static_cast<int>(message.size()), 0,
                              reinterpret_cast<const sockaddr*>(&destination),
                              sizeof(destination));
    return count == static_cast<int>(message.size()) ||
           (count < 0 && would_block(socket_error()));
}

void LanDiscovery::receive_available() noexcept {
    std::array<char, 256> buffer{};
    while (socket_ != -1) {
        sockaddr_in source{};
#if defined(_WIN32)
        int source_length = sizeof(source);
#else
        socklen_t source_length = sizeof(source);
#endif
        const auto count = recvfrom(as_socket(socket_), buffer.data(),
                                    static_cast<int>(buffer.size() - 1), 0,
                                    reinterpret_cast<sockaddr*>(&source),
                                    &source_length);
        if (count < 0) {
            if (!would_block(socket_error())) stop();
            return;
        }
        buffer[static_cast<std::size_t>(count)] = '\0';
        std::istringstream input(std::string{buffer.data(),
                                             static_cast<std::size_t>(count)});
        std::string magic;
        char type = 0;
        std::string fingerprint;
        if (!(input >> magic >> type >> fingerprint) ||
            magic != "GBB-DISCOVERY/1") {
            continue;
        }
        std::uint64_t parsed_fingerprint = 0;
        if (!parse_hex(fingerprint, parsed_fingerprint) ||
            parsed_fingerprint != rom_fingerprint_) {
            continue;
        }
        char address_text[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &source.sin_addr, address_text,
                      sizeof(address_text)) == nullptr) {
            continue;
        }
        if (mode_ == Mode::host && type == 'Q') {
            std::ostringstream response;
            response << "GBB-DISCOVERY/1 R " << fingerprint_text(rom_fingerprint_)
                     << ' ' << tcp_port_ << ' ' << name_;
            static_cast<void>(send_message(response.str(), address_text,
                                            ntohs(source.sin_port)));
            continue;
        }
        if (mode_ != Mode::scan || type != 'R') continue;
        unsigned port = 0;
        if (!(input >> port) || port == 0 || port > UINT16_MAX) continue;
        std::string name;
        std::getline(input, name);
        if (!name.empty() && name.front() == ' ') name.erase(0, 1);
        const auto duplicate = std::find_if(
            peers_.begin(), peers_.end(), [&](const LanPeer& peer) {
                return peer.address == address_text && peer.port == port;
            });
        if (duplicate == peers_.end()) {
            if (peers_.size() >= 64) continue;
            peers_.push_back({address_text, sanitize_name(std::move(name)),
                              static_cast<std::uint16_t>(port),
                              parsed_fingerprint});
        }
    }
}

} // namespace gameboy
