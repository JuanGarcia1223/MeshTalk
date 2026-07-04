#include "chat/ChatServer.h"
#include "crypto/KeyManager.h"
#include "crypto/SessionManager.h"
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

void run_info_editor(DatabaseManager& db) {
    while (true) {
        std::cout << "\n=== Personal Info Editor ===\n";
        std::cout << "1. Dump existing entries\n";
        std::cout << "2. Edit entry\n";
        std::cout << "3. Add entry\n";
        std::cout << "4. Delete entry\n";
        std::cout << "9. Exit\n";
        std::cout << "Choice: ";
        std::string choice;
        std::getline(std::cin, choice);

        if (choice == "1") {
            auto entries = db.loadAllInfoEntries();
            if (entries.empty()) {
                std::cout << "No entries found.\n";
            } else {
                std::cout << "\n  # | Key                | Value\n";
                std::cout << "----+--------------------+--------------------------------\n";
                for (size_t i = 0; i < entries.size(); ++i) {
                    std::cout << " " << std::setw(2) << (i + 1) << " | "
                              << std::setw(18) << std::left << entries[i].key.substr(0, 18) << " | "
                              << entries[i].value << "\n";
                }
            }
        } else if (choice == "2") {
            auto entries = db.loadAllInfoEntries();
            if (entries.empty()) {
                std::cout << "No entries to edit.\n";
                continue;
            }
            std::cout << "\n  # | Key                | Value\n";
            std::cout << "----+--------------------+--------------------------------\n";
            for (size_t i = 0; i < entries.size(); ++i) {
                std::cout << " " << std::setw(2) << (i + 1) << " | "
                          << std::setw(18) << std::left << entries[i].key.substr(0, 18) << " | "
                          << entries[i].value << "\n";
            }
            std::cout << "Enter index to edit: ";
            std::string idx_str;
            std::getline(std::cin, idx_str);
            try {
                size_t idx = std::stoul(idx_str) - 1;
                if (idx >= entries.size()) {
                    std::cout << "Invalid index.\n";
                    continue;
                }
                std::cout << "Current value: " << entries[idx].value << "\n";
                std::cout << "Enter new value: ";
                std::string new_value;
                std::getline(std::cin, new_value);
                if (db.saveInfoEntry(entries[idx].key, new_value)) {
                    std::cout << "Updated.\n";
                } else {
                    std::cout << "Failed to update.\n";
                }
            } catch (...) {
                std::cout << "Invalid index.\n";
            }
        } else if (choice == "3") {
            std::cout << "Enter key: ";
            std::string key;
            std::getline(std::cin, key);
            if (key.empty()) {
                std::cout << "Key cannot be empty.\n";
                continue;
            }
            std::cout << "Enter value: ";
            std::string value;
            std::getline(std::cin, value);
            if (db.saveInfoEntry(key, value)) {
                std::cout << "Added.\n";
            } else {
                std::cout << "Failed to add.\n";
            }
        } else if (choice == "4") {
            auto entries = db.loadAllInfoEntries();
            if (entries.empty()) {
                std::cout << "No entries to delete.\n";
                continue;
            }
            std::cout << "\n  # | Key                | Value\n";
            std::cout << "----+--------------------+--------------------------------\n";
            for (size_t i = 0; i < entries.size(); ++i) {
                std::cout << " " << std::setw(2) << (i + 1) << " | "
                          << std::setw(18) << std::left << entries[i].key.substr(0, 18) << " | "
                          << entries[i].value << "\n";
            }
            std::cout << "Enter index to delete: ";
            std::string idx_str;
            std::getline(std::cin, idx_str);
            try {
                size_t idx = std::stoul(idx_str) - 1;
                if (idx >= entries.size()) {
                    std::cout << "Invalid index.\n";
                    continue;
                }
                if (db.deleteInfoEntry(entries[idx].key)) {
                    std::cout << "Deleted.\n";
                } else {
                    std::cout << "Failed to delete.\n";
                }
            } catch (...) {
                std::cout << "Invalid index.\n";
            }
        } else if (choice == "9") {
            std::cout << "Exiting info editor.\n";
            break;
        } else {
            std::cout << "Invalid choice.\n";
        }
    }
}

}    // namespace

int main(int argc, char** argv) {
    bool debug_mode = false;
    bool no_ui_mode = false;
    bool udp_debug = false;
    bool verbose_crypto = false;
    bool edit_info_mode = false;
    std::string name;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--debug") == 0) {
            debug_mode = true;
        } else if (std::strcmp(argv[i], "--udp-debug") == 0) {
            udp_debug = true;
        } else if (std::strcmp(argv[i], "--verbose-crypto") == 0) {
            verbose_crypto = true;
        } else if (std::strcmp(argv[i], "--noui") == 0) {
            no_ui_mode = true;
        } else if (std::strcmp(argv[i], "--edit-info") == 0) {
            edit_info_mode = true;
        } else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else {
            std::cerr << "usage: " << argv[0]
                      << " --name <one-word-name> [--debug] [--verbose-crypto] [--udp-debug] [--noui] [--edit-info]\n";
            return 1;
        }
    }

    if (!is_one_word(name)) {
        std::cerr << "--name with a one-word value is required\n";
        return 1;
    }

    if (edit_info_mode) {
        DatabaseManager db_manager(name);
        if (!db_manager.init()) {
            std::cerr << "failed to initialize database\n";
            return 1;
        }
        run_info_editor(db_manager);
        return 0;
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
        // For noui mode, we need to initialize database and key manager
        DatabaseManager db_manager(name);
        if (!db_manager.init()) {
            std::cerr << "failed to initialize database\n";
            chat_server.stop();
            return 1;
        }
        KeyManager key_manager(db_manager);
        if (!key_manager.init()) {
            std::cerr << "failed to initialize crypto identity\n";
            chat_server.stop();
            return 1;
        }
        
        UdpHelloBroadcaster broadcaster(
            name, kDiscoveryUdpPort, chat_port, "", key_manager.publicKey(), {}, {}, udp_debug);
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

    // Get database reference for trust store operations
    DatabaseManager& db_manager = *ui.getDatabase();

    // Initialize crypto identity
    KeyManager key_manager(db_manager);
    if (!key_manager.init()) {
        std::cerr << "failed to initialize crypto identity\n";
        chat_server.stop();
        return 1;
    }

    // Initialize session manager for E2E encryption
    SessionManager session_manager(key_manager, db_manager);
    chat_server.set_session_manager(&session_manager);

    // Wire crypto logging to debug console when --verbose-crypto is set
    if (verbose_crypto) {
        auto crypto_logger = [&ui](const std::string& msg) { ui.add_debug(msg); };
        key_manager.set_logger(crypto_logger);
        session_manager.set_logger(crypto_logger);
        chat_server.set_logger(crypto_logger);
    }

    // Set up callbacks after all objects are initialized
    ui.set_on_trust_callback([&db_manager, &ui](const std::string& peer_name) {
        if (db_manager.trustPeer(peer_name)) {
            ui.add_debug("trusted peer: " + peer_name);
        }
    });
    
    ui.set_on_show_identity([&key_manager, &ui]() {
        ui.show_identity_popup(key_manager.myFingerprint());
    });

    // Set up file transfer callbacks
    chat_server.set_database(&db_manager);
    chat_server.set_trust_checker([&db_manager](const std::string& peer_name) -> bool {
        auto peer = db_manager.lookupPeer(peer_name);
        return peer.has_value() && peer->trust_status == "trusted";
    });
    
    ui.set_on_upload_file([&chat_server, &name, &ui](const std::string& filepath, const std::string& peer_name,
                                                      const std::string& peer_ip, uint16_t peer_port) -> bool {
        ui.add_debug("Starting upload of " + filepath + " to " + peer_name + " at " + peer_ip + ":" + std::to_string(peer_port));
        bool result = chat_server.send_file_offer(name, peer_name, peer_ip, peer_port, filepath);
        if (!result) {
            ui.add_debug("Failed to start upload to " + peer_name);
        }
        return result;
    });
    
    ui.set_on_download_file([&chat_server, &ui](const std::string& transfer_id, const std::string& download_path) -> bool {
        bool result = chat_server.download_file(transfer_id, download_path);
        return result;
    });

    // Set up file progress callback
    chat_server.set_file_progress_callback([&ui](const std::string& transfer_id, uint64_t bytes_sent, uint64_t total_bytes, bool complete) {
        ui.update_upload_progress(bytes_sent, complete);
    });

    // Set up file received callback
    chat_server.set_file_received_callback([&ui](const std::string& peer_name, bool is_sender,
                                                 const std::string& transfer_id, const std::string& filename,
                                                 uint64_t file_size) {
        if (peer_name.empty() || peer_name == "unknown") {
            ui.add_debug("file callback: invalid peer for transfer " + transfer_id + " filename=" + filename);
            return;
        }
        ui.add_attachment_message(peer_name, is_sender, filename, file_size, ui.local_datetime_now(), ui.unix_epoch_ms_now());
    });

    // Set up info request/response callbacks
    chat_server.set_info_received_callback([&ui, &db_manager](const std::string& peer_name,
                                                               const std::vector<std::pair<std::string, std::string>>& entries) {
        ui.add_debug("INFO: received response from " + peer_name + " entries=" + std::to_string(entries.size()));
        // Save to DB cache
        db_manager.clearPeerInfo(peer_name);
        for (const auto& e : entries) {
            db_manager.savePeerInfo(peer_name, e.first, e.second);
        }
        // If trust modal is showing for this peer, update it inline
        if (ui.is_showing_trust_modal_for(peer_name)) {
            ui.add_debug("INFO: updating trust modal for " + peer_name);
            ui.update_trust_modal_info(entries);
        } else {
            ui.add_debug("INFO: showing identity popup for " + peer_name);
            ui.show_identity_popup("", peer_name, entries);
        }
    });

    ui.set_on_request_info([&chat_server, &name, &ui](const std::string& peer_name) {
        // Look up peer IP and port from UI
        ui.add_debug("INFO: on_request_info callback for " + peer_name);
        auto peer_opt = ui.get_peer_info(peer_name);
        if (!peer_opt) {
            ui.add_debug("Cannot request info: peer " + peer_name + " not found");
            return;
        }
        auto peer = *peer_opt;
        if (peer.ip.empty() || peer.tcp_port == 0) {
            ui.add_debug("Cannot request info: peer " + peer_name + " is offline");
            return;
        }
        ui.add_debug("INFO: calling chat_server.request_info for " + peer_name + " at " + peer.ip + ":" + std::to_string(peer.tcp_port));
        chat_server.request_info(name, peer_name, peer.ip, peer.tcp_port);
    });

    UdpHelloBroadcaster broadcaster(
            name, kDiscoveryUdpPort, chat_port, "",
            key_manager.publicKey(),
            [&ui, &chat_server, &name, chat_port, &key_manager, &db_manager](const std::string& peer_name, const std::string& peer_ip,
                                uint16_t peer_port, const std::vector<uint8_t>& identity_pk) {
                if (peer_name == name && peer_port == chat_port) {
                    return;
                }
                
                // Trust decision logic
                std::string fingerprint = KeyManager::fingerprint(identity_pk);
                
                if (identity_pk.empty()) {
                    // Legacy peer without identity - treat as untrusted
                    ui.upsert_peer(peer_name, peer_ip, peer_port, false, true, fingerprint);
                    return;
                }
                
                auto existing = db_manager.lookupPeer(peer_name);
                if (!existing) {
                    // New peer - insert as pending
                    auto result = db_manager.upsertPeer(peer_name, identity_pk);
                    if (result == DatabaseManager::UpsertResult::Inserted) {
                        ui.upsert_peer(peer_name, peer_ip, peer_port, false, false, fingerprint);
                    }
                } else {
                    // Existing peer - check key match
                    auto result = db_manager.upsertPeer(peer_name, identity_pk);
                    if (result == DatabaseManager::UpsertResult::Mismatch) {
                        // Key mismatch - show warning, don't allow messaging
                        ui.upsert_peer(peer_name, peer_ip, peer_port, false, true, fingerprint);
                        ui.show_key_mismatch(peer_name, fingerprint,
                                            KeyManager::fingerprint(existing->public_key));
                    } else if (existing->trust_status == "trusted") {
                        // Trusted peer
                        ui.upsert_peer(peer_name, peer_ip, peer_port, true, false, fingerprint);
                        chat_server.register_peer(peer_name, peer_ip);
                    } else {
                        // Pending peer
                        ui.upsert_peer(peer_name, peer_ip, peer_port, false, false, fingerprint);
                    }
                }
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
        const std::string display_time = datetime.empty() ? TerminalUI::local_datetime_now() : datetime;
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
