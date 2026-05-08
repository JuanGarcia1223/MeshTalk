#include "chat/ChatServer.h"
#include "discovery/UdpHelloBroadcaster.h"
#include "ui/TerminalUI.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {
constexpr uint16_t kDiscoveryUdpPort = 55000;

std::atomic<bool> g_keep_running{true};

bool is_one_word(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    for (char c : value) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

void on_signal(int) { g_keep_running = false; }
}    // namespace

int main(int argc, char** argv) {
    bool debug_mode = false;
    bool no_ui_mode = false;
    std::string name;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--debug") == 0) {
            debug_mode = true;
        } else if (std::strcmp(argv[i], "--noui") == 0) {
            no_ui_mode = true;
        } else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else {
            std::cerr << "usage: " << argv[0] << " --name <one-word-name> [--debug] [--noui]\n";
            return 1;
        }
    }

    if (!is_one_word(name)) {
        std::cerr << "--name with a one-word value is required\n";
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    ChatServer chat_server;
    if (!chat_server.start()) {
        return 1;
    }
    const uint16_t chat_port = chat_server.port();

    if (no_ui_mode) {
        UdpHelloBroadcaster broadcaster(name, kDiscoveryUdpPort, chat_port, "127.0.0.1");
        if (!broadcaster.start()) {
            chat_server.stop();
            return 1;
        }
        std::cout << "noui mode started for '" << name << "'. press Ctrl+C to stop.\n";
        while (g_keep_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        broadcaster.stop();
        chat_server.stop();
        return 0;
    }

    TerminalUI ui(
            debug_mode, name,
            [&chat_server](const TerminalUI::PeerInfo& peer) {
                chat_server.connect_to(peer.ip, peer.tcp_port, peer.username);
            });
    if (!ui.init()) {
        chat_server.stop();
        return 1;
    }

    UdpHelloBroadcaster broadcaster(
            name, kDiscoveryUdpPort, chat_port, "127.0.0.1",
            [&ui](const std::string& peer_name, const std::string& peer_ip, uint16_t peer_port) {
                ui.upsert_peer(peer_name, peer_ip, peer_port);
            });
    if (!broadcaster.start()) {
        chat_server.stop();
        return 1;
    }
    ui.run();
    broadcaster.stop();
    chat_server.stop();
    return 0;
}
