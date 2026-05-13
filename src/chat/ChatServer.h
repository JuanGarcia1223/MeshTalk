#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <unordered_map>

class ChatServer {
public:
    ChatServer() = default;
    ~ChatServer();

    ChatServer(const ChatServer&) = delete;
    ChatServer& operator=(const ChatServer&) = delete;

    bool start();
    void stop();
    bool connect_to(const std::string& ip, uint16_t port, const std::string& peer_name);
    void register_peer(const std::string& peer_name, const std::string& ip);

    uint16_t port() const { return port_; }

private:
    void accept_loop();
    static std::string endpoint_key(const std::string& ip, uint16_t port);

    int listen_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex peers_mutex_;
    std::unordered_map<std::string, std::string> name_by_ip_;
    std::mutex outbound_mutex_;
    std::unordered_map<std::string, int> outbound_fd_by_endpoint_;
};
