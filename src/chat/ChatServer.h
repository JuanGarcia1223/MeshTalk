#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Forward declarations
class DatabaseManager;
class SessionManager;

enum class FileTransferStatus {
    PENDING,
    IN_PROGRESS,
    COMPLETE,
    FAILED,
    CANCELLED
};

struct OutgoingFileTransfer {
    std::string transfer_id;
    std::string filename;
    uint64_t file_size;
    std::string sha256_hash;
    std::string from_user;
    std::string peer_name;
    std::string ip;
    uint16_t port;
    std::vector<uint8_t> file_data;
    FileTransferStatus status;
    uint64_t bytes_sent;
    bool cancelled;
};

struct IncomingFileTransfer {
    std::string transfer_id;
    std::string filename;
    uint64_t file_size;
    std::string sha256_hash;
    std::string from_user;
    std::vector<uint8_t> chunks;
    uint32_t expected_chunks;
    uint32_t received_chunks;
    FileTransferStatus status;
    int64_t timestamp_ms;
    std::string iso_datetime;
};

class ChatServer {
public:
    ChatServer();
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
    void set_delivery_callback(std::function<void(const std::string&, const std::string&, bool)> callback);

    uint16_t port() const { return port_; }

    // File transfer methods
    void set_database(DatabaseManager* db);
    void set_file_progress_callback(std::function<void(const std::string&, uint64_t, uint64_t, bool)> callback);
    void set_file_received_callback(
        std::function<void(const std::string&, bool, const std::string&, const std::string&, uint64_t)> callback);
    void set_trust_checker(std::function<bool(const std::string&)> fn) { is_peer_trusted_ = std::move(fn); }
    void set_session_manager(SessionManager* sm) { session_manager_ = sm; }
    void set_logger(std::function<void(const std::string&)> fn) { logger_ = std::move(fn); }
    
    bool send_file_offer(const std::string& from_user, const std::string& to_user,
                         const std::string& ip, uint16_t port, const std::string& filepath);
    void cancel_file_transfer(const std::string& transfer_id);
    bool download_file(const std::string& transfer_id, const std::string& download_path);

private:
    void accept_loop();
    void handle_inbound_connection(int fd, const std::string& peer_ip, uint16_t peer_port);
    static std::string endpoint_key(const std::string& ip, uint16_t port);
    std::string resolve_peer_name(const std::string& peer_ip);
    std::string session_key_for_ip(const std::string& peer_ip);
    int get_outbound_fd(const std::string& ip, uint16_t port);
    void handle_chat_message(const std::string& from_user, const class ChatMessage& msg);
    void handle_delivery_ack(const std::string& from_user, const class DeliveryAck& ack);

    // File transfer helpers
    void send_file_chunks(const std::string& transfer_id);
    void handle_file_offer(const std::string& from_user, const std::string& ip, uint16_t port, const class FileOffer& offer);
    void handle_file_chunk(const std::string& from_user, const class FileChunk& chunk);
    void handle_file_complete(const std::string& from_user, const class FileComplete& complete);
    void send_file_response(const std::string& to_user, const std::string& ip, uint16_t port, 
                            const std::string& transfer_id, bool accepted);
    std::string compute_sha256(const std::vector<uint8_t>& data);

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
    std::mutex delivery_callback_mutex_;
    std::function<void(const std::string&, const std::string&, bool)> on_delivery_;

    // File transfer state
    DatabaseManager* db_{nullptr};
    std::mutex file_transfers_mutex_;
    std::unordered_map<std::string, std::shared_ptr<OutgoingFileTransfer>> outgoing_transfers_;
    std::unordered_map<std::string, std::shared_ptr<IncomingFileTransfer>> incoming_transfers_;
    std::function<void(const std::string&, uint64_t, uint64_t, bool)> file_progress_callback_;
    std::function<void(const std::string&, bool, const std::string&, const std::string&, uint64_t)>
        file_received_callback_;
    std::function<bool(const std::string&)> is_peer_trusted_;
    SessionManager* session_manager_{nullptr};
    std::mutex session_keys_mutex_;
    std::unordered_map<std::string, std::string> ip_to_session_key_;
    std::function<void(const std::string&)> logger_;
};
