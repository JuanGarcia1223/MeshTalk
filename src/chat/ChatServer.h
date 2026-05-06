#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

class ChatServer {
public:
    ChatServer() = default;
    ~ChatServer();

    ChatServer(const ChatServer&) = delete;
    ChatServer& operator=(const ChatServer&) = delete;

    bool start();
    void stop();

    uint16_t port() const { return port_; }

private:
    void accept_loop();

    int listen_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
};
