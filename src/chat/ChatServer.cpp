#include "chat/ChatServer.h"

#include "chat.pb.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

ChatServer::~ChatServer() { stop(); }

std::string ChatServer::endpoint_key(const std::string& ip, uint16_t port) {
    return ip + ":" + std::to_string(port);
}

namespace {
bool send_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = send(fd, p + sent, len - sent, 0);
#endif
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    size_t got = 0;
    while (got < len) {
        const ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

int64_t unix_epoch_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string iso_utc_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}
}  // namespace

bool ChatServer::start() {
    if (running_) {
        return true;
    }

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "chat: failed to create socket\n";
        return false;
    }

    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(0);    // Let OS allocate an available port.
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "chat: bind failed\n";
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &bound_len) < 0) {
        std::cerr << "chat: getsockname failed\n";
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    port_ = ntohs(bound.sin_port);

    if (listen(listen_fd_, 16) < 0) {
        std::cerr << "chat: listen failed\n";
        close(listen_fd_);
        listen_fd_ = -1;
        port_ = 0;
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&ChatServer::accept_loop, this);
    std::cout << "chat: listening on tcp port " << port_ << "\n";
    return true;
}

void ChatServer::stop() {
    running_ = false;
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        for (auto& kv : outbound_fd_by_endpoint_) {
            if (kv.second >= 0) {
                close(kv.second);
            }
        }
        outbound_fd_by_endpoint_.clear();
    }
    port_ = 0;
}

bool ChatServer::connect_to(const std::string& ip, uint16_t port, const std::string& peer_name) {
    const std::string key = endpoint_key(ip, port);
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        auto it = outbound_fd_by_endpoint_.find(key);
        if (it != outbound_fd_by_endpoint_.end() && it->second >= 0) {
            std::cout << "chat: already connected to " << peer_name << " (" << key << ")\n";
            return true;
        }
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "chat: connect socket create failed for " << peer_name << "\n";
        return false;
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &dst.sin_addr) != 1) {
        std::cerr << "chat: invalid peer IP '" << ip << "' for " << peer_name << "\n";
        close(fd);
        return false;
    }

    std::cout << "chat: connecting to " << peer_name << " at " << ip << ":" << port << "\n";
    if (connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) < 0) {
        std::cerr << "chat: connect failed to " << peer_name << " (" << ip << ":" << port
                  << ")\n";
        close(fd);
        return false;
    }

    std::cout << "chat: connected to " << peer_name << " (" << ip << ":" << port << ")\n";
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        outbound_fd_by_endpoint_[key] = fd;
    }
    return true;
}

bool ChatServer::send_chat(const std::string& from_user, const std::string& to_user,
                           const std::string& ip, uint16_t port, const std::string& content) {
    if (content.empty()) {
        return false;
    }
    if (!connect_to(ip, port, to_user)) {
        return false;
    }

    ChatMessage msg;
    const uint64_t seq = ++msg_counter_;
    msg.set_msg_id(from_user + "-" + std::to_string(unix_epoch_ms()) + "-" + std::to_string(seq));
    msg.set_from_user(from_user);
    msg.set_to_user(to_user);
    msg.set_content(content);
    msg.set_timestamp_ms(unix_epoch_ms());
    msg.set_iso_datetime(iso_utc_now());

    std::string payload;
    if (!msg.SerializeToString(&payload)) {
        std::cerr << "chat: failed to serialize chat message\n";
        return false;
    }

    const uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    const std::string key = endpoint_key(ip, port);

    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        auto it = outbound_fd_by_endpoint_.find(key);
        if (it == outbound_fd_by_endpoint_.end()) {
            return false;
        }
        fd = it->second;
    }

    if (!send_all(fd, &len_be, sizeof(len_be)) || !send_all(fd, payload.data(), payload.size())) {
        std::cerr << "chat: send failed to " << to_user << " (" << key << ")\n";
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        auto it = outbound_fd_by_endpoint_.find(key);
        if (it != outbound_fd_by_endpoint_.end()) {
            close(it->second);
            outbound_fd_by_endpoint_.erase(it);
        }
        return false;
    }

    std::cout << "chat: sent msg_id=" << msg.msg_id() << " to=" << to_user
              << " at " << ip << ":" << port << " ts=" << msg.iso_datetime() << "\n";
    return true;
}

void ChatServer::register_peer(const std::string& peer_name, const std::string& ip) {
    if (peer_name.empty() || ip.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(peers_mutex_);
    name_by_ip_[ip] = peer_name;
}

void ChatServer::accept_loop() {
    while (running_) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            if (running_) {
                std::cerr << "chat: accept failed\n";
            }
            continue;
        }

        char ipbuf[INET_ADDRSTRLEN];
        const char* peer_ip = inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof(ipbuf));
        const std::string ip = (peer_ip ? peer_ip : "unknown");
        std::string peer_name = "unknown";
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            auto it = name_by_ip_.find(ip);
            if (it != name_by_ip_.end()) {
                peer_name = it->second;
            }
        }
        const uint16_t peer_port = ntohs(peer.sin_port);
        std::cout << "chat: accepted connection from " << ip << ":" << peer_port
                            << " name=" << peer_name << "\n";
        std::thread(&ChatServer::handle_inbound_connection, this, fd, ip, peer_port).detach();
    }
}

void ChatServer::handle_inbound_connection(int fd, const std::string& peer_ip, uint16_t peer_port) {
    // Ensure accepted chat sockets block for reads; do not inherit short listener timeout.
    timeval no_timeout{};
    no_timeout.tv_sec = 0;
    no_timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout));

    while (running_) {
        uint32_t len_be = 0;
        if (!recv_all(fd, &len_be, sizeof(len_be))) {
            break;
        }
        const uint32_t len = ntohl(len_be);
        if (len == 0 || len > 4 * 1024 * 1024) {
            std::cerr << "chat: invalid inbound payload length " << len << "\n";
            break;
        }

        std::string payload(len, '\0');
        if (!recv_all(fd, payload.data(), len)) {
            break;
        }

        ChatMessage msg;
        if (!msg.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
            std::cerr << "chat: failed to parse inbound chat payload from "
                      << peer_ip << ":" << peer_port << "\n";
            continue;
        }

        std::cout << "chat: recv from=" << msg.from_user()
                  << " to=" << msg.to_user()
                  << " msg_id=" << msg.msg_id()
                  << " ts=" << msg.iso_datetime()
                  << " content=\"" << msg.content() << "\"\n";
    }

    close(fd);
}
