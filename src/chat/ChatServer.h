#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
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
    bool send_chat(const std::string& from_user, const std::string& to_user,
                   const std::string& ip, uint16_t port, const std::string& content);
    void register_peer(const std::string& peer_name, const std::string& ip);
    void disconnect_peer(const std::string& peer_name);
    void set_receive_handler(std::function<void(const std::string&, const std::string&, const std::string&,
                                                const std::string&, int64_t)> handler);

    uint16_t port() const { return port_; }

private:
    void accept_loop();
    void handle_inbound_connection(int fd, const std::string& peer_ip, uint16_t peer_port);
    static std::string endpoint_key(const std::string& ip, uint16_t port);

    int listen_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex peers_mutex_;
    std::unordered_map<std::string, std::string> name_by_ip_;
    std::mutex outbound_mutex_;
    std::unordered_map<std::string, int> outbound_fd_by_endpoint_;
    std::atomic<uint64_t> msg_counter_{0};
    std::mutex receive_handler_mutex_;
    std::function<void(const std::string&, const std::string&, const std::string&, const std::string&, int64_t)>
        on_receive_;
};
