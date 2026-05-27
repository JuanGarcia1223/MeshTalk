#pragma once

#include <SQLiteCpp/SQLiteCpp.h>

#include <memory>
#include <mutex>
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

    std::vector<std::string> getAllPeers();

private:
    std::string getDbPath() const;
    bool ensureDirectoryExists(const std::string& path);
    bool createSchema();

    std::string username_;
    std::unique_ptr<SQLite::Database> db_;
    std::mutex mutex_;
};
