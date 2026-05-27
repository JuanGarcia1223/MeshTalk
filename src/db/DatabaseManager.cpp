#include "db/DatabaseManager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>

DatabaseManager::DatabaseManager(const std::string& username) : username_(username) {}

std::string DatabaseManager::getDbPath() const {
    const char* home = std::getenv("HOME");
    if (!home) {
        home = "/tmp";
    }
    return std::string(home) + "/.local/share/meshtalk/" + username_ + ".db";
}

bool DatabaseManager::ensureDirectoryExists(const std::string& path) {
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string dir = path.substr(0, pos);
        if (dir.empty() || dir == "/") continue;
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) {
            mkdir(dir.c_str(), 0755);
        }
    }
    // Create the final directory if needed
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        mkdir(path.c_str(), 0755);
    }
    return true;
}

bool DatabaseManager::init() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string db_path = getDbPath();
    std::string dir = db_path.substr(0, db_path.find_last_of('/'));
    ensureDirectoryExists(dir);

    try {
        db_ = std::make_unique<SQLite::Database>(db_path,
                                                  SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        return createSchema();
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to open database at " << db_path << ": " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::createSchema() {
    try {
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS messages (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                peer_name TEXT NOT NULL,
                is_sender INTEGER NOT NULL,
                content TEXT NOT NULL,
                timestamp TEXT NOT NULL,
                timestamp_ms INTEGER NOT NULL,
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP
            );
        )");

        db_->exec(R"(
            CREATE INDEX IF NOT EXISTS idx_messages_peer ON messages(peer_name);
        )");

        db_->exec(R"(
            CREATE INDEX IF NOT EXISTS idx_messages_timestamp_ms ON messages(timestamp_ms);
        )");

        // Schema migration: add timestamp_ms column if it doesn't exist (for old databases)
        try {
            SQLite::Statement check(*db_, "SELECT timestamp_ms FROM messages LIMIT 1");
            check.executeStep();
        } catch (...) {
            // Column doesn't exist, add it
            db_->exec("ALTER TABLE messages ADD COLUMN timestamp_ms INTEGER DEFAULT 0");
            std::cout << "db: migrated schema - added timestamp_ms column\n";
        }

        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to create schema: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::saveMessage(const std::string& peer_name, bool is_sender,
                                  const std::string& content, const std::string& timestamp,
                                  int64_t timestamp_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement insert(*db_,
                                 "INSERT INTO messages (peer_name, is_sender, content, timestamp, timestamp_ms) "
                                 "VALUES (?, ?, ?, ?, ?)");
        insert.bind(1, peer_name);
        insert.bind(2, is_sender ? 1 : 0);
        insert.bind(3, content);
        insert.bind(4, timestamp);
        insert.bind(5, timestamp_ms);
        insert.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to save message: " << ex.what() << "\n";
        return false;
    }
}

std::vector<ChatMessageRecord> DatabaseManager::loadAllMessages() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<ChatMessageRecord> records;

    if (!db_) {
        return records;
    }

    try {
        SQLite::Statement query(*db_,
                                "SELECT id, peer_name, is_sender, content, timestamp, timestamp_ms "
                                "FROM messages ORDER BY timestamp_ms ASC");

        while (query.executeStep()) {
            ChatMessageRecord record;
            record.id = query.getColumn(0).getInt64();
            record.peer_name = query.getColumn(1).getString();
            record.is_sender = query.getColumn(2).getInt() != 0;
            record.content = query.getColumn(3).getString();
            record.timestamp = query.getColumn(4).getString();
            record.timestamp_ms = query.getColumn(5).getInt64();
            records.push_back(record);
        }
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to load messages: " << ex.what() << "\n";
    }

    return records;
}

std::vector<ChatMessageRecord> DatabaseManager::loadMessagesForPeer(const std::string& peer_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<ChatMessageRecord> records;

    if (!db_) {
        return records;
    }

    try {
        SQLite::Statement query(*db_,
                                "SELECT id, peer_name, is_sender, content, timestamp, timestamp_ms "
                                "FROM messages WHERE peer_name = ? ORDER BY timestamp_ms ASC");
        query.bind(1, peer_name);

        while (query.executeStep()) {
            ChatMessageRecord record;
            record.id = query.getColumn(0).getInt64();
            record.peer_name = query.getColumn(1).getString();
            record.is_sender = query.getColumn(2).getInt() != 0;
            record.content = query.getColumn(3).getString();
            record.timestamp = query.getColumn(4).getString();
            record.timestamp_ms = query.getColumn(5).getInt64();
            records.push_back(record);
        }
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to load messages for peer: " << ex.what() << "\n";
    }

    return records;
}

std::vector<std::string> DatabaseManager::getAllPeers() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> peers;

    if (!db_) {
        return peers;
    }

    try {
        SQLite::Statement query(*db_,
                                "SELECT DISTINCT peer_name FROM messages ORDER BY peer_name");

        while (query.executeStep()) {
            peers.push_back(query.getColumn(0).getString());
        }
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to get peers: " << ex.what() << "\n";
    }

    return peers;
}
