#include "gameboy/lan_discovery.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <iptypes.h>
#else
#include <arpa/inet.h>
#if defined(__ANDROID__)
#include <dlfcn.h>
#endif
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <vector>

namespace gameboy {
namespace {

// Broadcast delivery is disabled by a number of Wi-Fi access points and
// Android network stacks. Keep it as a compatibility path, but use a scoped
// administratively-local multicast group as the primary LAN query channel.
constexpr const char* discovery_multicast_address = "239.255.42.99";

#if defined(__ANDROID__)
// getifaddrs was added to the Android NDK at API 24, while GBB keeps a
// minSdk of 21. Resolve it at runtime so current devices can select the Wi-Fi
// interface without making older supported devices fail to load the library.
struct AndroidIfaddrsApi {
    using get_fn = int (*)(ifaddrs**);
    using free_fn = void (*)(ifaddrs*);

    void* library{};
    get_fn get{};
    free_fn free{};

    AndroidIfaddrsApi() noexcept {
        library = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) return;
        get = reinterpret_cast<get_fn>(dlsym(library, "getifaddrs"));
        free = reinterpret_cast<free_fn>(dlsym(library, "freeifaddrs"));
        if (get == nullptr || free == nullptr) {
            dlclose(library);
            library = nullptr;
            get = nullptr;
            free = nullptr;
        }
    }

    ~AndroidIfaddrsApi() {
        if (library != nullptr) dlclose(library);
    }

    AndroidIfaddrsApi(const AndroidIfaddrsApi&) = delete;
    AndroidIfaddrsApi& operator=(const AndroidIfaddrsApi&) = delete;
};
#endif

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

std::string profile_text(const LinkCompatibilityProfile profile) {
    if (!profile.known()) return {};
    std::ostringstream output;
    output << "profile=" << static_cast<unsigned>(profile.version) << '-'
           << static_cast<unsigned>(profile.generation) << '-'
           << static_cast<unsigned>(profile.region) << '-'
           << static_cast<unsigned>(profile.modes);
    return output.str();
}

bool parse_profile_text(const std::string& text,
                        LinkCompatibilityProfile& profile) noexcept {
    if (text.rfind("profile=", 0) != 0) return false;
    std::istringstream input(text.substr(8));
    unsigned version = 0, generation = 0, region = 0, modes = 0;
    char separator = 0;
    if (!(input >> version >> separator) || separator != '-' ||
        !(input >> generation >> separator) || separator != '-' ||
        !(input >> region >> separator) || separator != '-' ||
        !(input >> modes) || version > UINT8_MAX || generation > UINT8_MAX ||
        region > UINT8_MAX || modes > UINT8_MAX) {
        return false;
    }
    profile = {static_cast<std::uint8_t>(version),
               static_cast<LinkGeneration>(generation),
               static_cast<LinkRegion>(region),
               static_cast<std::uint8_t>(modes)};
    return profile.known();
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
                              const std::uint64_t compatibility_id,
                              const std::uint64_t rom_fingerprint,
                              const std::string& name,
                              const LinkCompatibilityProfile profile) noexcept {
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
    int broadcast = 1;
    static_cast<void>(setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
                                 reinterpret_cast<const char*>(&broadcast),
                                 sizeof(broadcast)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(discovery_port);
    if (bind(socket, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0) {
        close_socket(socket);
        return false;
    }
    ip_mreq membership{};
    const auto multicast_ready =
        inet_pton(AF_INET, discovery_multicast_address,
                  &membership.imr_multiaddr) == 1;
    bool multicast_joined = false;
#if defined(_WIN32)
    if (multicast_ready) {
        membership.imr_interface.s_addr = htonl(INADDR_ANY);
        multicast_joined = setsockopt(
                                socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                reinterpret_cast<const char*>(&membership),
                                sizeof(membership)) == 0;
    }
#else
    // Android and Linux may select a non-Wi-Fi route for INADDR_ANY. Join on
    // every active non-loopback IPv4 interface so a Windows scanner's
    // multicast query is received regardless of the device's route table.
    if (multicast_ready) {
        ifaddrs* interfaces = nullptr;
#if defined(__ANDROID__)
        const AndroidIfaddrsApi ifaddrs_api;
        const auto interfaces_loaded = ifaddrs_api.get != nullptr &&
                                        ifaddrs_api.get(&interfaces) == 0;
#else
        const auto interfaces_loaded = getifaddrs(&interfaces) == 0;
#endif
        if (interfaces_loaded) {
            for (auto* entry = interfaces; entry != nullptr;
                 entry = entry->ifa_next) {
                if (entry->ifa_addr == nullptr ||
                    entry->ifa_addr->sa_family != AF_INET ||
                    (entry->ifa_flags & IFF_LOOPBACK) != 0 ||
                    (entry->ifa_flags & IFF_UP) == 0) {
                    continue;
                }
                membership.imr_interface =
                    reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr;
                if (setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                               reinterpret_cast<const char*>(&membership),
                               sizeof(membership)) == 0) {
                    multicast_joined = true;
                }
            }
#if defined(__ANDROID__)
            ifaddrs_api.free(interfaces);
#else
            freeifaddrs(interfaces);
#endif
        }
        if (!multicast_joined) {
            membership.imr_interface.s_addr = htonl(INADDR_ANY);
            multicast_joined = setsockopt(
                                   socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                   reinterpret_cast<const char*>(&membership),
                                   sizeof(membership)) == 0;
        }
    }
#endif
    static_cast<void>(multicast_joined);
    // Joining is best effort: broadcast remains a compatibility path on
    // networks that reject multicast. Bind first because Android/Linux
    // require the local port to be selected before multicast delivery is
    // reliable.
    socket_ = as_handle(socket);
    mode_ = Mode::host;
    tcp_port_ = tcp_port;
    compatibility_id_ = compatibility_id;
    rom_fingerprint_ = rom_fingerprint;
    compatibility_profile_ = profile;
    name_ = sanitize_name(name);
    next_host_advertisement_ = std::chrono::steady_clock::now();
    return true;
}

bool LanDiscovery::start_host(const std::uint16_t tcp_port,
                              const std::uint64_t rom_fingerprint,
                              const std::string& name) noexcept {
    return start_host(tcp_port, rom_fingerprint, rom_fingerprint, name);
}

bool LanDiscovery::start_scan(const std::uint64_t compatibility_id,
                              const std::uint64_t rom_fingerprint,
                              const LinkCompatibilityProfile profile) noexcept {
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
    // Prefer the well-known port so unsolicited host advertisements have a
    // destination that scanners can receive. A host and scanner may also run
    // on the same machine (the contract test does this), so fall back to an
    // ephemeral port when the discovery port is already occupied.
    address.sin_port = htons(discovery_port);
    if (bind(socket, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0) {
        close_socket(socket);
        const auto fallback = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fallback == invalid_socket || !set_nonblocking(fallback)) {
            if (fallback != invalid_socket) close_socket(fallback);
            return false;
        }
        static_cast<void>(setsockopt(
            fallback, SOL_SOCKET, SO_BROADCAST,
            reinterpret_cast<const char*>(&broadcast), sizeof(broadcast)));
        address.sin_port = 0;
        if (bind(fallback, reinterpret_cast<const sockaddr*>(&address),
                 sizeof(address)) != 0) {
            close_socket(fallback);
            return false;
        }
        socket_ = as_handle(fallback);
    } else {
        socket_ = as_handle(socket);
    }
    mode_ = Mode::scan;
    compatibility_id_ = compatibility_id;
    rom_fingerprint_ = rom_fingerprint;
    compatibility_profile_ = profile;
    peers_.clear();
    scan_message_ = std::string("GBB-DISCOVERY/1 Q ") +
                    fingerprint_text(compatibility_id_);
    if (const auto descriptor = profile_text(compatibility_profile_);
        !descriptor.empty()) {
        scan_message_ += ' ' + descriptor;
    }
    // A broadcast can be rejected by a host firewall even when ordinary
    // unicast UDP is available. Keep the scanner alive so the loopback and
    // any later interface-specific responses can still be consumed.
    const auto broadcast_sent =
        send_message(scan_message_, "255.255.255.255", discovery_port);
    const auto directed_broadcast_sent =
        send_directed_broadcasts(scan_message_);
    const auto subnet_probe_sent = send_subnet_probes(scan_message_);
    const auto multicast_sent =
        send_message(scan_message_, discovery_multicast_address, discovery_port);
    // Loopback makes discovery testable and covers hosts where broadcast is
    // filtered by the local firewall; it does not replace the LAN broadcast.
    const auto loopback_sent =
        send_message(scan_message_, "127.0.0.1", discovery_port);
    if (!broadcast_sent && !directed_broadcast_sent && !subnet_probe_sent &&
        !multicast_sent &&
        !loopback_sent) {
        stop();
        return false;
    }
    return true;
}

bool LanDiscovery::start_scan(const std::uint64_t rom_fingerprint) noexcept {
    return start_scan(rom_fingerprint, rom_fingerprint);
}

void LanDiscovery::poll() noexcept {
    if (socket_ == -1) return;
    const auto now = std::chrono::steady_clock::now();
    if (mode_ == Mode::host && !name_.empty() &&
        (next_host_advertisement_ == std::chrono::steady_clock::time_point{} ||
         now >= next_host_advertisement_)) {
        std::ostringstream response;
        response << "GBB-DISCOVERY/1 R "
                 << fingerprint_text(compatibility_id_) << ' '
                 << fingerprint_text(rom_fingerprint_) << ' ' << tcp_port_
                 << ' ';
        if (const auto descriptor = profile_text(compatibility_profile_);
            !descriptor.empty()) {
            response << descriptor << ' ';
        }
        response << name_;
        const auto message = response.str();
        // Advertise periodically as well as answering queries. This handles
        // networks that drop broadcast/multicast packets in one direction
        // (notably Windows scanners discovering Android hosts).
        static_cast<void>(send_message(message, "255.255.255.255",
                                        discovery_port));
        static_cast<void>(send_directed_broadcasts(message));
        static_cast<void>(send_message(message, discovery_multicast_address,
                                       discovery_port));
        next_host_advertisement_ = now + std::chrono::milliseconds(250);
    }
    if (mode_ == Mode::scan && !scan_message_.empty()) {
        if (next_scan_broadcast_ == std::chrono::steady_clock::time_point{} ||
            now >= next_scan_broadcast_) {
            // Repeat the query while the bounded scan is active. Broadcast
            // packets are routinely dropped by Wi-Fi access points and
            // firewalls; a short retry window makes discovery reliable
            // without blocking the emulation thread.
            static_cast<void>(send_message(scan_message_,
                                            "255.255.255.255",
                                            discovery_port));
            static_cast<void>(send_directed_broadcasts(scan_message_));
            static_cast<void>(send_subnet_probes(scan_message_));
            static_cast<void>(send_message(scan_message_,
                                           discovery_multicast_address,
                                           discovery_port));
            static_cast<void>(send_message(scan_message_, "127.0.0.1",
                                            discovery_port));
            next_scan_broadcast_ = now + std::chrono::milliseconds(200);
        }
    }
    receive_available();
}

void LanDiscovery::stop() noexcept {
    if (socket_ != -1) close_socket(as_socket(socket_));
    socket_ = -1;
    peers_.clear();
    name_.clear();
    tcp_port_ = 0;
    compatibility_id_ = 0;
    rom_fingerprint_ = 0;
    compatibility_profile_ = {};
    scan_message_.clear();
    next_scan_broadcast_ = {};
    next_host_advertisement_ = {};
}

std::vector<LanPeer> LanDiscovery::take_peers() {
    auto peers = std::move(peers_);
    peers_.clear();
    return peers;
}

bool LanDiscovery::send_directed_broadcasts(
    const std::string& message) noexcept {
#if defined(_WIN32)
    // Some access points and Windows firewall profiles discard the limited
    // broadcast address (255.255.255.255) but still deliver a subnet-directed
    // broadcast. Query every active IPv4 adapter so Android hosts can be
    // discovered without requiring users to enter their address manually.
    ULONG buffer_size = 15U * 1024U;
    std::vector<unsigned char> buffer(buffer_size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    auto result = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
        addresses, &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        if (buffer_size == 0 || buffer_size > 1024U * 1024U) return false;
        buffer.resize(buffer_size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addresses, &buffer_size);
    }
    if (result != NO_ERROR) return false;

    bool sent = false;
    for (auto* adapter = addresses; adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        for (auto* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET ||
                unicast->OnLinkPrefixLength > 32) {
                continue;
            }
            const auto* address = reinterpret_cast<const sockaddr_in*>(
                unicast->Address.lpSockaddr);
            const auto host = ntohl(address->sin_addr.s_addr);
            if ((host >> 24U) == 127U) continue;
            const auto prefix = unicast->OnLinkPrefixLength;
            const auto mask = prefix == 0
                                  ? 0U
                                  : 0xffffffffU << (32U - prefix);
            const auto broadcast = htonl(host | ~mask);
            char text[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &broadcast, text, sizeof(text)) == nullptr) {
                continue;
            }
            sent = send_message(message, text, discovery_port) || sent;
        }
    }
    return sent;
#elif !defined(__ANDROID__)
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0 || interfaces == nullptr) return false;
    bool sent = false;
    for (auto* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_netmask == nullptr ||
            entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & IFF_LOOPBACK) != 0 ||
            (entry->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        const auto* address = reinterpret_cast<const sockaddr_in*>(
            entry->ifa_addr);
        const auto* netmask = reinterpret_cast<const sockaddr_in*>(
            entry->ifa_netmask);
        const auto host = ntohl(address->sin_addr.s_addr);
        const auto mask = ntohl(netmask->sin_addr.s_addr);
        if (mask == 0 || mask == std::numeric_limits<std::uint32_t>::max()) {
            continue;
        }
        const auto broadcast = htonl(host | ~mask);
        char text[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &broadcast, text, sizeof(text)) != nullptr) {
            sent = send_message(message, text, discovery_port) || sent;
        }
    }
    freeifaddrs(interfaces);
    return sent;
#else
    // Android's minSdk is below the API level where getifaddrs is available
    // at link time. Resolve it through libc, as the multicast join path does.
    const AndroidIfaddrsApi ifaddrs_api;
    if (ifaddrs_api.get == nullptr || ifaddrs_api.free == nullptr) return false;
    ifaddrs* interfaces = nullptr;
    if (ifaddrs_api.get(&interfaces) != 0 || interfaces == nullptr) return false;
    bool sent = false;
    for (auto* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_netmask == nullptr ||
            entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & IFF_LOOPBACK) != 0 ||
            (entry->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        const auto* address = reinterpret_cast<const sockaddr_in*>(
            entry->ifa_addr);
        const auto* netmask = reinterpret_cast<const sockaddr_in*>(
            entry->ifa_netmask);
        const auto host = ntohl(address->sin_addr.s_addr);
        const auto mask = ntohl(netmask->sin_addr.s_addr);
        if (mask == 0 || mask == std::numeric_limits<std::uint32_t>::max()) {
            continue;
        }
        const auto broadcast = htonl(host | ~mask);
        char text[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &broadcast, text, sizeof(text)) != nullptr) {
            sent = send_message(message, text, discovery_port) || sent;
        }
    }
    ifaddrs_api.free(interfaces);
    return sent;
#endif
}

bool LanDiscovery::send_subnet_probes(const std::string& message) noexcept {
#if defined(_WIN32)
    // If a firewall or access point filters broadcasts, probe the local
    // subnet with ordinary unicast UDP. Replies then follow the outbound
    // flow and are commonly permitted even when unsolicited broadcasts are
    // blocked. Limit probing to practical LAN sizes; directed broadcasts and
    // multicast remain the paths for larger networks.
    ULONG buffer_size = 15U * 1024U;
    std::vector<unsigned char> buffer(buffer_size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    auto result = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
        addresses, &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        if (buffer_size == 0 || buffer_size > 1024U * 1024U) return false;
        buffer.resize(buffer_size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addresses, &buffer_size);
    }
    if (result != NO_ERROR) return false;

    bool sent = false;
    for (auto* adapter = addresses; adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        for (auto* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET ||
                unicast->OnLinkPrefixLength < 16 ||
                unicast->OnLinkPrefixLength > 30) {
                continue;
            }
            const auto* address = reinterpret_cast<const sockaddr_in*>(
                unicast->Address.lpSockaddr);
            const auto host = ntohl(address->sin_addr.s_addr);
            const auto prefix = unicast->OnLinkPrefixLength;
            const auto mask = 0xffffffffU << (32U - prefix);
            const auto network = host & mask;
            const auto broadcast = network | ~mask;
            // Avoid generating traffic for the local machine and cap each
            // interface at 4094 destinations (the common /24 is complete).
            const auto first = network + 1U;
            const auto last = broadcast - 1U;
            if (last < first || last - first > 4093U) continue;
            for (auto candidate = first; candidate <= last; ++candidate) {
                if (candidate == host) continue;
                in_addr destination{};
                destination.s_addr = htonl(candidate);
                char text[INET_ADDRSTRLEN]{};
                if (inet_ntop(AF_INET, &destination, text, sizeof(text)) !=
                    nullptr) {
                    sent = send_message(message, text, discovery_port) || sent;
                }
            }
        }
    }
    return sent;
#else
    static_cast<void>(message);
    return false;
#endif
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
        std::string compatibility;
        if (!(input >> magic >> type >> compatibility) ||
            magic != "GBB-DISCOVERY/1") {
            continue;
        }
        std::uint64_t parsed_compatibility = 0;
        if (!parse_hex(compatibility, parsed_compatibility) ||
            parsed_compatibility != compatibility_id_) {
            continue;
        }
        char address_text[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &source.sin_addr, address_text,
                      sizeof(address_text)) == nullptr) {
            continue;
        }
        if (mode_ == Mode::host && type == 'Q') {
            std::string token;
            LinkCompatibilityProfile peer_profile{};
            const auto has_profile = static_cast<bool>(input >> token) &&
                                     parse_profile_text(token, peer_profile);
            if (compatibility_profile_.known() && has_profile &&
                !link_profiles_compatible(compatibility_profile_, peer_profile)) {
                continue;
            }
            std::ostringstream response;
            response << "GBB-DISCOVERY/1 R "
                     << fingerprint_text(compatibility_id_) << ' '
                     << fingerprint_text(rom_fingerprint_) << ' ' << tcp_port_
                     << ' ';
            if (const auto descriptor = profile_text(compatibility_profile_);
                !descriptor.empty()) {
                response << descriptor << ' ';
            }
            response << name_;
            static_cast<void>(send_message(response.str(), address_text,
                                            ntohs(source.sin_port)));
            continue;
        }
        if (mode_ != Mode::scan || type != 'R') continue;
        std::string fingerprint;
        if (!(input >> fingerprint)) continue;
        std::uint64_t parsed_fingerprint = 0;
        if (!parse_hex(fingerprint, parsed_fingerprint)) continue;
        unsigned port = 0;
        if (!(input >> port) || port == 0 || port > UINT16_MAX) continue;
        std::string first_name;
        if (!(input >> first_name)) continue;
        LinkCompatibilityProfile peer_profile{};
        if (parse_profile_text(first_name, peer_profile)) {
            if (compatibility_profile_.known() &&
                !link_profiles_compatible(compatibility_profile_, peer_profile)) {
                continue;
            }
            first_name.clear();
        }
        std::string name;
        std::getline(input, name);
        if (!first_name.empty()) name = ' ' + first_name + name;
        if (!name.empty() && name.front() == ' ') name.erase(0, 1);
        const auto duplicate = std::find_if(
            peers_.begin(), peers_.end(), [&](const LanPeer& peer) {
                return peer.address == address_text && peer.port == port;
            });
        if (duplicate == peers_.end()) {
            if (peers_.size() >= 64) continue;
            peers_.push_back({address_text, sanitize_name(std::move(name)),
                              static_cast<std::uint16_t>(port),
                              parsed_compatibility,
                              parsed_fingerprint,
                              peer_profile});
        }
    }
}

} // namespace gameboy
