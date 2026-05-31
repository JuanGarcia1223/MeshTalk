#pragma once

#include "discovery.pb.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class UdpHelloBroadcaster {
public:
    UdpHelloBroadcaster(std::string username, uint16_t udp_port, uint16_t tcp_port,
                            std::string payload_ip = "127.0.0.1",
                            std::vector<uint8_t> identity_pk = {},
                            std::function<void(const std::string&, const std::string&, uint16_t,
                                               const std::vector<uint8_t>&)>
                                    on_peer_seen = {},
                            std::function<void(const std::string&)> on_peer_bye = {},
                            bool debug_logs = false);
    ~UdpHelloBroadcaster();

    UdpHelloBroadcaster(const UdpHelloBroadcaster&) = delete;
    UdpHelloBroadcaster& operator=(const UdpHelloBroadcaster&) = delete;

    bool start();
    void stop();
    void send_bye();

private:
    void run_broadcast_loop();
    void run_receive_loop();
    bool send_hello();
    bool send_packet(DiscoveryPacket::Type type);

    std::string username_;
    uint16_t udp_port_;
    uint16_t tcp_port_;
    std::string payload_ip_;
    std::vector<uint8_t> identity_pk_;
    std::function<void(const std::string&, const std::string&, uint16_t,
                       const std::vector<uint8_t>&)> on_peer_seen_;
    std::function<void(const std::string&)> on_peer_bye_;
    bool debug_logs_{false};

    int send_sockfd_{-1};
    int recv_sockfd_{-1};
    std::atomic<bool> running_{false};
    std::thread broadcaster_thread_;
    std::thread receiver_thread_;
};
