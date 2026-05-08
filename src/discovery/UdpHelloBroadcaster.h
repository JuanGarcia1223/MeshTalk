#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

class UdpHelloBroadcaster {
public:
    UdpHelloBroadcaster(std::string username, uint16_t udp_port, uint16_t tcp_port,
                            std::string payload_ip = "127.0.0.1",
                            std::function<void(const std::string&, const std::string&, uint16_t)>
                                    on_peer_seen = {},
                            bool debug_logs = false);
    ~UdpHelloBroadcaster();

    UdpHelloBroadcaster(const UdpHelloBroadcaster&) = delete;
    UdpHelloBroadcaster& operator=(const UdpHelloBroadcaster&) = delete;

    bool start();
    void stop();

private:
    void run_broadcast_loop();
    void run_receive_loop();
    bool send_hello();

    std::string username_;
    uint16_t udp_port_;
    uint16_t tcp_port_;
    std::string payload_ip_;
    std::function<void(const std::string&, const std::string&, uint16_t)> on_peer_seen_;
    bool debug_logs_{false};

    int send_sockfd_{-1};
    int recv_sockfd_{-1};
    std::atomic<bool> running_{false};
    std::thread broadcaster_thread_;
    std::thread receiver_thread_;
};
