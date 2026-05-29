#include "chat/ChatServer.h"
#include "discovery/UdpHelloBroadcaster.h"
#include "ui/TerminalUI.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
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

std::string local_datetime_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}
}    // namespace

int main(int argc, char** argv) {
    bool debug_mode = false;
    bool no_ui_mode = false;
    bool udp_debug = false;
    std::string name;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--debug") == 0) {
            debug_mode = true;
        } else if (std::strcmp(argv[i], "--udp-debug") == 0) {
            udp_debug = true;
        } else if (std::strcmp(argv[i], "--noui") == 0) {
            no_ui_mode = true;
        } else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else {
            std::cerr << "usage: " << argv[0]
                      << " --name <one-word-name> [--debug] [--udp-debug] [--noui]\n";
            return 1;
        }
    }

    if (!is_one_word(name)) {
        std::cerr << "--name with a one-word value is required\n";
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    ChatServer chat_server;
    if (!chat_server.start()) {
        return 1;
    }
    const uint16_t chat_port = chat_server.port();

    if (no_ui_mode) {
        UdpHelloBroadcaster broadcaster(
            name, kDiscoveryUdpPort, chat_port, "127.0.0.1", {}, {}, udp_debug);
        if (!broadcaster.start()) {
            chat_server.stop();
            return 1;
        }
        std::cout << "noui mode started for '" << name << "'. press Ctrl+C to stop.\n";
        while (g_keep_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        broadcaster.send_bye();
        broadcaster.stop();
        chat_server.stop();
        return 0;
    }

    TerminalUI ui(
            debug_mode, name,
            [&chat_server, &name, chat_port](const TerminalUI::PeerInfo& peer) {
                if (peer.username == name && peer.tcp_port == chat_port) {
                    std::cout << "chat: ignoring self-connect to " << peer.username << "\n";
                    return;
                }
                chat_server.connect_to(peer.ip, peer.tcp_port, peer.username);
            },
            [&chat_server, &name](const TerminalUI::PeerInfo& peer, const std::string& text) {
                return chat_server.send_chat(name, peer.username, peer.ip, peer.tcp_port, text);
            },
            [&chat_server](const std::string& peer_name) {
                chat_server.disconnect_peer(peer_name);
            });
    if (!ui.init()) {
        chat_server.stop();
        return 1;
    }
    if (!ui.initDatabase()) {
        chat_server.stop();
        return 1;
    }

    UdpHelloBroadcaster broadcaster(
            name, kDiscoveryUdpPort, chat_port, "127.0.0.1",
            [&ui, &chat_server, &name, chat_port](const std::string& peer_name, const std::string& peer_ip,
                                uint16_t peer_port) {
                if (peer_name == name && peer_port == chat_port) {
                    return;
                }
                ui.upsert_peer(peer_name, peer_ip, peer_port);
                chat_server.register_peer(peer_name, peer_ip);
            },
            [&ui, &chat_server](const std::string& peer_name) {
                // Received BYE from peer - mark offline immediately
                ui.mark_peer_offline(peer_name);
                chat_server.disconnect_peer(peer_name);
            },
            udp_debug);
    chat_server.set_receive_handler([&ui, &name](const std::string& from_user, const std::string& to_user,
                                                  const std::string& content, const std::string& datetime,
                                                  int64_t timestamp_ms) {
        if (to_user != name) {
            return;
        }
        const std::string display_time = datetime.empty() ? local_datetime_now() : datetime;
        ui.add_chat_message(from_user, false, content, display_time, timestamp_ms);
    });
    if (!broadcaster.start()) {
        chat_server.stop();
        return 1;
    }
    ui.run();
    broadcaster.send_bye();
    broadcaster.stop();
    chat_server.stop();
    return 0;
}
