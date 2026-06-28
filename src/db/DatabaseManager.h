#pragma once

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct ChatMessageRecord {
    int64_t id;
    std::string msg_id;
    std::string peer_name;
    bool is_sender;
    std::string content;
    std::string timestamp;
    int64_t timestamp_ms;
    std::string status;  // "pending", "sent", "delivered", "read"
};

struct PeerIdentity {
    std::string username;
    std::vector<uint8_t> public_key;
    std::string trust_status;  // "trusted" or "pending"
    int64_t first_seen;
};

struct FileTransferRecord {
    int64_t id;
    std::string transfer_id;  // UUID for the transfer
    std::string filename;
    uint64_t file_size;
    std::string sha256_hash;
    std::string peer_name;
    bool is_sender;
    std::string status;  // "pending", "in_progress", "complete", "failed", "cancelled"
    std::string timestamp;
    int64_t timestamp_ms;
    std::string file_data;  // BLOB stored as string for SQLite
};

struct InfoEntry {
    int64_t id;
    std::string key;
    std::string value;
    int64_t updated_at;
};

class DatabaseManager {
public:
    DatabaseManager(const std::string& username);
    ~DatabaseManager() = default;

    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    bool init();

    bool saveMessage(const std::string& peer_name, bool is_sender,
                     const std::string& content, const std::string& timestamp,
                     int64_t timestamp_ms,
                     const std::string& status = "sent",
                     const std::string& msg_id = "");
    bool updateMessageStatus(const std::string& msg_id, const std::string& status);
    bool updateMessageStatusByContent(const std::string& peer_name, const std::string& content,
                                       const std::string& status);

    std::vector<ChatMessageRecord> loadAllMessages();
    std::vector<ChatMessageRecord> loadMessagesForPeer(const std::string& peer_name);
    bool clearMessagesForPeer(const std::string& peer_name);

    std::vector<std::string> getAllPeers();

    // Unread counts
    int getUnreadCount(const std::string& peer_name);
    bool setUnreadCount(const std::string& peer_name, int count);
    bool incrementUnreadCount(const std::string& peer_name);
    bool clearUnreadCount(const std::string& peer_name);
    bool markMessagesAsRead(const std::string& peer_name);

    // Identity methods
    bool saveIdentity(const std::vector<uint8_t>& public_key, const std::vector<uint8_t>& private_key);
    bool loadIdentity(std::vector<uint8_t>& public_key, std::vector<uint8_t>& private_key);

    // Known peers (trust store) methods
    std::optional<PeerIdentity> lookupPeer(const std::string& username);
    enum class UpsertResult { Inserted, Updated, Mismatch };
    UpsertResult upsertPeer(const std::string& username, const std::vector<uint8_t>& public_key);
    bool trustPeer(const std::string& username);
    std::vector<PeerIdentity> getAllKnownPeers();

    // File transfer methods
    bool saveFileTransfer(const std::string& transfer_id, const std::string& filename,
                          uint64_t file_size, const std::string& sha256_hash,
                          const std::string& peer_name, bool is_sender,
                          const std::string& status, const std::string& timestamp,
                          int64_t timestamp_ms);
    bool updateFileTransferStatus(const std::string& transfer_id, const std::string& status);
    bool saveFileData(const std::string& transfer_id, const std::vector<uint8_t>& data);
    std::optional<std::vector<uint8_t>> loadFileData(const std::string& transfer_id);
    std::vector<FileTransferRecord> loadAllFileTransfers();
    std::vector<FileTransferRecord> loadFileTransfersForPeer(const std::string& peer_name);
    std::optional<FileTransferRecord> getFileTransfer(const std::string& transfer_id);
    std::string getFileStoragePath() const;

    // Personal info entries (key-value pairs broadcast to peers)
    bool saveInfoEntry(const std::string& key, const std::string& value);
    bool deleteInfoEntry(const std::string& key);
    bool updateInfoEntry(const std::string& key, const std::string& value);
    std::vector<InfoEntry> loadAllInfoEntries();
    std::optional<std::string> getInfoEntry(const std::string& key);

    // Peer info cache (info received from other peers)
    bool savePeerInfo(const std::string& peer_name, const std::string& key, const std::string& value);
    bool clearPeerInfo(const std::string& peer_name);
    std::vector<InfoEntry> loadPeerInfo(const std::string& peer_name);

private:
    std::string getDbPath() const;
    bool ensureDirectoryExists(const std::string& path);
    bool createSchema();

    std::string username_;
    std::unique_ptr<SQLite::Database> db_;
    std::mutex mutex_;
};
