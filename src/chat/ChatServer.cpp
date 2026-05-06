#include "chat/ChatServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

ChatServer::~ChatServer() { stop(); }

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
    port_ = 0;
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
        std::cout << "chat: accepted connection from "
                            << (peer_ip ? peer_ip : "unknown") << ":" << ntohs(peer.sin_port)
                            << "\n";
        close(fd);
    }
}
