#include "discovery/UdpHelloBroadcaster.h"

#include "discovery.pb.h"

#include <arpa/inet.h>
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
}  // namespace

UdpHelloBroadcaster::UdpHelloBroadcaster(std::string username, uint16_t udp_port,
                                         uint16_t tcp_port, std::string payload_ip,
                                         std::function<void(const std::string&)> on_peer_seen)
    : username_(std::move(username)),
      udp_port_(udp_port),
      tcp_port_(tcp_port),
      payload_ip_(std::move(payload_ip)),
      on_peer_seen_(std::move(on_peer_seen)) {}

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

    char ipbuf[INET_ADDRSTRLEN];
    const char* src_ip = inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf));
    std::cout << "received packet type=" << pkt.type() << " username=" << pkt.username()
              << " ip=" << pkt.ip() << " tcp_port=" << pkt.tcp_port()
              << " from=" << (src_ip ? src_ip : "unknown")
              << ":" << ntohs(from.sin_port) << "\n";
    if (on_peer_seen_ && !pkt.username().empty()) {
      on_peer_seen_(pkt.username());
    }
  }
}

bool UdpHelloBroadcaster::send_hello() {
  DiscoveryPacket pkt;
  pkt.set_type(DiscoveryPacket::HELLO);
  pkt.set_username(username_);
  pkt.set_ip(payload_ip_);
  pkt.set_tcp_port(tcp_port_);
  pkt.set_timestamp(unix_epoch_ms());

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

  std::cout << "broadcast HELLO username=" << username_ << " ip=" << payload_ip_
            << " tcp_port=" << tcp_port_ << " udp_port=" << udp_port_ << "\n";
  return true;
}
