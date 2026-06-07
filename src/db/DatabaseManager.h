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
    std::string peer_name;
    bool is_sender;
    std::string content;
    std::string timestamp;
    int64_t timestamp_ms;
};

struct PeerIdentity {
    std::string username;
    std::vector<uint8_t> public_key;
    std::string trust_status;  // "trusted" or "pending"
    int64_t first_seen;
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
                     int64_t timestamp_ms);

    std::vector<ChatMessageRecord> loadAllMessages();
    std::vector<ChatMessageRecord> loadMessagesForPeer(const std::string& peer_name);
    bool clearMessagesForPeer(const std::string& peer_name);

    std::vector<std::string> getAllPeers();

    // Identity methods
    bool saveIdentity(const std::vector<uint8_t>& public_key, const std::vector<uint8_t>& private_key);
    bool loadIdentity(std::vector<uint8_t>& public_key, std::vector<uint8_t>& private_key);

    // Known peers (trust store) methods
    std::optional<PeerIdentity> lookupPeer(const std::string& username);
    enum class UpsertResult { Inserted, Updated, Mismatch };
    UpsertResult upsertPeer(const std::string& username, const std::vector<uint8_t>& public_key);
    bool trustPeer(const std::string& username);
    std::vector<PeerIdentity> getAllKnownPeers();

private:
    std::string getDbPath() const;
    bool ensureDirectoryExists(const std::string& path);
    bool createSchema();

    std::string username_;
    std::unique_ptr<SQLite::Database> db_;
    std::mutex mutex_;
};
