#include "discovery/UdpHelloBroadcaster.h"

#include "discovery.pb.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {
int64_t unix_epoch_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string get_local_ip() {
    struct ifaddrs *ifaddr, *ifa;
    char addr_str[INET_ADDRSTRLEN];
    std::string found_ip;

    if (getifaddrs(&ifaddr) == -1) {
        return "127.0.0.1";
    }

    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;  // Only IPv4
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;  // Skip loopback
        if (!(ifa->ifa_flags & IFF_UP)) continue;  // Skip down interfaces

        void *addr_ptr = &reinterpret_cast<sockaddr_in*>(ifa->ifa_addr)->sin_addr;
        if (inet_ntop(AF_INET, addr_ptr, addr_str, sizeof(addr_str))) {
            found_ip = addr_str;
            // Prefer interfaces that look like main network (eth, en, wlan)
            std::string name = ifa->ifa_name;
            if (name.find("eth") == 0 || name.find("en") == 0 || 
                name.find("wlan") == 0 || name.find("wlp") == 0) {
                freeifaddrs(ifaddr);
                return found_ip;
            }
        }
    }

    freeifaddrs(ifaddr);
    return found_ip.empty() ? "127.0.0.1" : found_ip;
}
}    // namespace

UdpHelloBroadcaster::UdpHelloBroadcaster(std::string username, uint16_t udp_port,
                                     uint16_t tcp_port, std::string payload_ip,
                                     std::vector<uint8_t> identity_pk,
                                     std::function<void(const std::string&, const std::string&, uint16_t,
                                                        const std::vector<uint8_t>&)>
                                             on_peer_seen,
                                     std::function<void(const std::string&)> on_peer_bye,
                                     bool debug_logs)
        : username_(std::move(username)),
            udp_port_(udp_port),
            tcp_port_(tcp_port),
            identity_pk_(std::move(identity_pk)),
            on_peer_seen_(std::move(on_peer_seen)),
            on_peer_bye_(std::move(on_peer_bye)),
            debug_logs_(debug_logs) {
    // Auto-detect LAN IP if not provided or localhost
    if (payload_ip.empty() || payload_ip == "127.0.0.1" || payload_ip == "localhost") {
        payload_ip_ = get_local_ip();
        if (debug_logs_) {
            std::cout << "udp: auto-detected IP: " << payload_ip_ << "\n";
        }
    } else {
        payload_ip_ = std::move(payload_ip);
    }
}

UdpHelloBroadcaster::~UdpHelloBroadcaster() { stop(); }

bool UdpHelloBroadcaster::start() {
    if (running_) {
        return true;
    }

    send_sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_sockfd_ < 0) {
        std::cerr << "failed to create UDP send socket\n";
        return false;
    }

    int enable_broadcast = 1;
    if (setsockopt(send_sockfd_, SOL_SOCKET, SO_BROADCAST, &enable_broadcast,
                                 sizeof(enable_broadcast)) < 0) {
        std::cerr << "failed to enable SO_BROADCAST\n";
        close(send_sockfd_);
        send_sockfd_ = -1;
        return false;
    }

    recv_sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_sockfd_ < 0) {
        std::cerr << "failed to create UDP receive socket\n";
        close(send_sockfd_);
        send_sockfd_ = -1;
        return false;
    }

    int reuse = 1;
    setsockopt(recv_sockfd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(recv_sockfd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    setsockopt(recv_sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(udp_port_);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(recv_sockfd_, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        std::cerr << "failed to bind UDP receive socket on port " << udp_port_ << "\n";
        close(send_sockfd_);
        close(recv_sockfd_);
        send_sockfd_ = -1;
        recv_sockfd_ = -1;
        return false;
    }

    running_ = true;
    broadcaster_thread_ = std::thread(&UdpHelloBroadcaster::run_broadcast_loop, this);
    receiver_thread_ = std::thread(&UdpHelloBroadcaster::run_receive_loop, this);
    return true;
}

void UdpHelloBroadcaster::stop() {
    running_ = false;
    if (broadcaster_thread_.joinable()) {
        broadcaster_thread_.join();
    }
    if (receiver_thread_.joinable()) {
        receiver_thread_.join();
    }
    if (send_sockfd_ >= 0) {
        close(send_sockfd_);
        send_sockfd_ = -1;
    }
    if (recv_sockfd_ >= 0) {
        close(recv_sockfd_);
        recv_sockfd_ = -1;
    }
}

void UdpHelloBroadcaster::run_broadcast_loop() {
    while (running_) {
        if (!send_hello()) {
            std::cerr << "failed to send HELLO broadcast\n";
        }
        for (int i = 0; i < 50 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void UdpHelloBroadcaster::run_receive_loop() {
    while (running_) {
        char buffer[4096];
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const ssize_t n = recvfrom(recv_sockfd_, buffer, sizeof(buffer), 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) {
            continue;
        }

        DiscoveryPacket pkt;
        if (!pkt.ParseFromArray(buffer, static_cast<int>(n))) {
            std::cerr << "received non-protobuf packet len=" << n << "\n";
            continue;
        }

        // Ignore our own discovery broadcasts.
        if (pkt.username() == username_ && pkt.tcp_port() == tcp_port_) {
            continue;
        }

        if (debug_logs_) {
            char ipbuf[INET_ADDRSTRLEN];
            const char* src_ip = inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf));
            std::cout << "received packet type=" << pkt.type() << " username=" << pkt.username()
                            << " ip=" << pkt.ip() << " tcp_port=" << pkt.tcp_port()
                            << " from=" << (src_ip ? src_ip : "unknown")
                            << ":" << ntohs(from.sin_port) << "\n";
        }

        if (pkt.type() == DiscoveryPacket::BYE) {
            // Peer is going offline immediately
            if (on_peer_bye_ && !pkt.username().empty()) {
                on_peer_bye_(pkt.username());
            }
        } else if (on_peer_seen_ && !pkt.username().empty() && !pkt.ip().empty()) {
            // Extract identity public key if present
            std::vector<uint8_t> peer_identity_pk;
            if (!pkt.identity_pk().empty()) {
                peer_identity_pk.assign(pkt.identity_pk().begin(), pkt.identity_pk().end());
            }
            on_peer_seen_(pkt.username(), pkt.ip(), static_cast<uint16_t>(pkt.tcp_port()), peer_identity_pk);
        }
    }
}

bool UdpHelloBroadcaster::send_packet(DiscoveryPacket::Type type) {
    DiscoveryPacket pkt;
    pkt.set_type(type);
    pkt.set_username(username_);
    pkt.set_ip(payload_ip_);
    pkt.set_tcp_port(tcp_port_);
    pkt.set_timestamp(unix_epoch_ms());
    
    // Include identity public key if available
    if (!identity_pk_.empty()) {
        pkt.set_identity_pk(identity_pk_.data(), identity_pk_.size());
    }

    std::string payload;
    if (!pkt.SerializeToString(&payload)) {
        return false;
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(udp_port_);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    const ssize_t sent = sendto(send_sockfd_, payload.data(), payload.size(), 0,
                                                            reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    if (sent < 0) {
        return false;
    }

    if (debug_logs_) {
        const char* type_str = (type == DiscoveryPacket::HELLO) ? "HELLO" :
                               (type == DiscoveryPacket::BYE) ? "BYE" : "HEARTBEAT";
        std::cout << "broadcast " << type_str << " username=" << username_ << " ip=" << payload_ip_
                        << " tcp_port=" << tcp_port_ << " udp_port=" << udp_port_ << "\n";
    }
    return true;
}

bool UdpHelloBroadcaster::send_hello() {
    return send_packet(DiscoveryPacket::HELLO);
}

void UdpHelloBroadcaster::send_bye() {
    if (send_sockfd_ < 0) {
        return;
    }
    // Send BYE multiple times to ensure delivery
    for (int i = 0; i < 3; ++i) {
        send_packet(DiscoveryPacket::BYE);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (debug_logs_) {
        std::cout << "sent BYE broadcast (3x) for clean shutdown\n";
    }
}
