#include "db/DatabaseManager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <cstdlib>
#include <ctime>
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
                msg_id TEXT,
                peer_name TEXT NOT NULL,
                is_sender INTEGER NOT NULL,
                content TEXT NOT NULL,
                timestamp TEXT NOT NULL,
                timestamp_ms INTEGER NOT NULL,
                status TEXT NOT NULL DEFAULT 'sent' CHECK (status IN ('pending', 'sent', 'delivered', 'read')),
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP
            );
        )");

        db_->exec(R"(
            CREATE INDEX IF NOT EXISTS idx_messages_peer ON messages(peer_name);
        )");

        db_->exec(R"(
            CREATE INDEX IF NOT EXISTS idx_messages_timestamp_ms ON messages(timestamp_ms);
        )");

        // Schema migration: add columns if they don't exist (for old databases)
        try {
            SQLite::Statement check(*db_, "SELECT timestamp_ms FROM messages LIMIT 1");
            check.executeStep();
        } catch (...) {
            db_->exec("ALTER TABLE messages ADD COLUMN timestamp_ms INTEGER DEFAULT 0");
            std::cout << "db: migrated schema - added timestamp_ms column\n";
        }
        try {
            SQLite::Statement check(*db_, "SELECT status FROM messages LIMIT 1");
            check.executeStep();
        } catch (...) {
            db_->exec("ALTER TABLE messages ADD COLUMN status TEXT DEFAULT 'sent'");
            db_->exec("ALTER TABLE messages ADD COLUMN msg_id TEXT");
            std::cout << "db: migrated schema - added status and msg_id columns\n";
        }

        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS unread_counts (
                peer_name TEXT PRIMARY KEY,
                count INTEGER NOT NULL DEFAULT 0
            );
        )");

        // Identity table - stores this user's Ed25519 keypair
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS identity (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                public_key BLOB NOT NULL,
                private_key BLOB NOT NULL,
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP
            );
        )");

        // Known peers table - trust store for other users
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS known_peers (
                username TEXT PRIMARY KEY,
                public_key BLOB NOT NULL,
                trust_status TEXT NOT NULL CHECK (trust_status IN ('trusted', 'pending')),
                first_seen INTEGER NOT NULL,
                last_seen INTEGER NOT NULL,
                updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
            );
        )");

        db_->exec(R"(
            CREATE INDEX IF NOT EXISTS idx_known_peers_status ON known_peers(trust_status);
        )");

        // File transfers table
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS file_transfers (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                transfer_id TEXT UNIQUE NOT NULL,
                filename TEXT NOT NULL,
                file_size INTEGER NOT NULL,
                sha256_hash TEXT,
                peer_name TEXT NOT NULL,
                is_sender INTEGER NOT NULL,
                status TEXT NOT NULL CHECK (status IN ('pending', 'in_progress', 'complete', 'failed', 'cancelled')),
                timestamp TEXT NOT NULL,
                timestamp_ms INTEGER NOT NULL,
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP
            );
        )");

        db_->exec(R"(
            CREATE INDEX IF NOT EXISTS idx_file_transfers_peer ON file_transfers(peer_name);
        )");

        db_->exec(R"(
            CREATE INDEX IF NOT EXISTS idx_file_transfers_timestamp ON file_transfers(timestamp_ms);
        )");

        // File data table (stores actual file content)
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS file_data (
                transfer_id TEXT PRIMARY KEY,
                data BLOB NOT NULL,
                FOREIGN KEY (transfer_id) REFERENCES file_transfers(transfer_id) ON DELETE CASCADE
            );
        )");

        // Personal info entries table (key-value pairs for this user)
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS info_entries (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL,
                updated_at INTEGER NOT NULL
            );
        )");

        // Peer info cache table (info received from other peers)
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS peer_info (
                peer_name TEXT NOT NULL,
                key TEXT NOT NULL,
                value TEXT NOT NULL,
                updated_at INTEGER NOT NULL,
                PRIMARY KEY (peer_name, key)
            );
        )");

        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to create schema: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::saveMessage(const std::string& peer_name, bool is_sender,
                                  const std::string& content, const std::string& timestamp,
                                  int64_t timestamp_ms,
                                  const std::string& status,
                                  const std::string& msg_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement insert(*db_,
                                 "INSERT INTO messages (msg_id, peer_name, is_sender, content, timestamp, timestamp_ms, status) "
                                 "VALUES (?, ?, ?, ?, ?, ?, ?)");
        insert.bind(1, msg_id.empty() ? nullptr : msg_id.c_str());
        insert.bind(2, peer_name);
        insert.bind(3, is_sender ? 1 : 0);
        insert.bind(4, content);
        insert.bind(5, timestamp);
        insert.bind(6, timestamp_ms);
        insert.bind(7, status);
        insert.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to save message: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::updateMessageStatus(const std::string& msg_id, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_ || msg_id.empty()) {
        return false;
    }

    try {
        SQLite::Statement update(*db_, "UPDATE messages SET status = ? WHERE msg_id = ?");
        update.bind(1, status);
        update.bind(2, msg_id);
        update.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to update message status: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::updateMessageStatusByContent(const std::string& peer_name,
                                                    const std::string& content,
                                                    const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement update(*db_,
            "UPDATE messages SET status = ? WHERE peer_name = ? AND content = ? AND status != 'read' "
            "ORDER BY id DESC LIMIT 1");
        update.bind(1, status);
        update.bind(2, peer_name);
        update.bind(3, content);
        update.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to update message status by content: " << ex.what() << "\n";
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
                                "SELECT id, msg_id, peer_name, is_sender, content, timestamp, timestamp_ms, status "
                                "FROM messages ORDER BY timestamp_ms ASC");

        while (query.executeStep()) {
            ChatMessageRecord record;
            record.id = query.getColumn(0).getInt64();
            record.msg_id = query.getColumn(1).getText();
            record.peer_name = query.getColumn(2).getString();
            record.is_sender = query.getColumn(3).getInt() != 0;
            record.content = query.getColumn(4).getString();
            record.timestamp = query.getColumn(5).getString();
            record.timestamp_ms = query.getColumn(6).getInt64();
            record.status = query.getColumn(7).getText();
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
                                "SELECT id, msg_id, peer_name, is_sender, content, timestamp, timestamp_ms, status "
                                "FROM messages WHERE peer_name = ? ORDER BY timestamp_ms ASC");
        query.bind(1, peer_name);

        while (query.executeStep()) {
            ChatMessageRecord record;
            record.id = query.getColumn(0).getInt64();
            record.msg_id = query.getColumn(1).getText();
            record.peer_name = query.getColumn(2).getString();
            record.is_sender = query.getColumn(3).getInt() != 0;
            record.content = query.getColumn(4).getString();
            record.timestamp = query.getColumn(5).getString();
            record.timestamp_ms = query.getColumn(6).getInt64();
            record.status = query.getColumn(7).getText();
            records.push_back(record);
        }
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to load messages for peer: " << ex.what() << "\n";
    }

    return records;
}

bool DatabaseManager::clearMessagesForPeer(const std::string& peer_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement query(*db_, "DELETE FROM messages WHERE peer_name = ?");
        query.bind(1, peer_name);
        query.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to clear messages for peer: " << ex.what() << "\n";
        return false;
    }
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

bool DatabaseManager::saveIdentity(const std::vector<uint8_t>& public_key,
                                   const std::vector<uint8_t>& private_key) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement insert(*db_,
            "INSERT OR REPLACE INTO identity (id, public_key, private_key) VALUES (1, ?, ?)");
        insert.bind(1, public_key.data(), static_cast<int>(public_key.size()));
        insert.bind(2, private_key.data(), static_cast<int>(private_key.size()));
        insert.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to save identity: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::loadIdentity(std::vector<uint8_t>& public_key,
                                   std::vector<uint8_t>& private_key) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement query(*db_, "SELECT public_key, private_key FROM identity WHERE id = 1");
        if (query.executeStep()) {
            const void* pub_data = query.getColumn(0).getBlob();
            int pub_size = query.getColumn(0).getBytes();
            const void* priv_data = query.getColumn(1).getBlob();
            int priv_size = query.getColumn(1).getBytes();

            public_key.resize(pub_size);
            private_key.resize(priv_size);
            std::memcpy(public_key.data(), pub_data, pub_size);
            std::memcpy(private_key.data(), priv_data, priv_size);
            return true;
        }
        return false;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to load identity: " << ex.what() << "\n";
        return false;
    }
}

std::optional<PeerIdentity> DatabaseManager::lookupPeer(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return std::nullopt;
    }

    try {
        SQLite::Statement query(*db_,
            "SELECT username, public_key, trust_status, first_seen FROM known_peers WHERE username = ?");
        query.bind(1, username);

        if (query.executeStep()) {
            PeerIdentity peer;
            peer.username = query.getColumn(0).getString();

            const void* key_data = query.getColumn(1).getBlob();
            int key_size = query.getColumn(1).getBytes();
            peer.public_key.resize(key_size);
            std::memcpy(peer.public_key.data(), key_data, key_size);

            peer.trust_status = query.getColumn(2).getString();
            peer.first_seen = query.getColumn(3).getInt64();
            return peer;
        }
        return std::nullopt;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to lookup peer: " << ex.what() << "\n";
        return std::nullopt;
    }
}

DatabaseManager::UpsertResult DatabaseManager::upsertPeer(const std::string& username,
                                                          const std::vector<uint8_t>& public_key) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return UpsertResult::Mismatch;  // Error case
    }

    try {
        // Check if peer exists
        SQLite::Statement check(*db_,
            "SELECT public_key FROM known_peers WHERE username = ?");
        check.bind(1, username);

        if (check.executeStep()) {
            // Peer exists - check if key matches
            const void* stored_key_data = check.getColumn(0).getBlob();
            int stored_key_size = check.getColumn(0).getBytes();

            if (static_cast<size_t>(stored_key_size) != public_key.size() ||
                std::memcmp(stored_key_data, public_key.data(), public_key.size()) != 0) {
                // Key mismatch!
                std::cout << "db: key mismatch for peer " << username << "\n";
                return UpsertResult::Mismatch;
            }

            // Key matches - update last_seen
            SQLite::Statement update(*db_,
                "UPDATE known_peers SET last_seen = ? WHERE username = ?");
            update.bind(1, static_cast<int64_t>(std::time(nullptr)));
            update.bind(2, username);
            update.exec();
            return UpsertResult::Updated;
        }

        // New peer - insert as pending
        SQLite::Statement insert(*db_,
            "INSERT INTO known_peers (username, public_key, trust_status, first_seen, last_seen) "
            "VALUES (?, ?, 'pending', ?, ?)");
        insert.bind(1, username);
        insert.bind(2, public_key.data(), static_cast<int>(public_key.size()));
        int64_t now = std::time(nullptr);
        insert.bind(3, now);
        insert.bind(4, now);
        insert.exec();
        std::cout << "db: new peer " << username << " added as pending\n";
        return UpsertResult::Inserted;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to upsert peer: " << ex.what() << "\n";
        return UpsertResult::Mismatch;  // Error case
    }
}

bool DatabaseManager::trustPeer(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement update(*db_,
            "UPDATE known_peers SET trust_status = 'trusted' WHERE username = ?");
        update.bind(1, username);
        update.exec();
        std::cout << "db: peer " << username << " trusted\n";
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to trust peer: " << ex.what() << "\n";
        return false;
    }
}

std::vector<PeerIdentity> DatabaseManager::getAllKnownPeers() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<PeerIdentity> peers;

    if (!db_) {
        return peers;
    }

    try {
        SQLite::Statement query(*db_,
            "SELECT username, public_key, trust_status, first_seen FROM known_peers "
            "ORDER BY trust_status DESC, username ASC");

        while (query.executeStep()) {
            PeerIdentity peer;
            peer.username = query.getColumn(0).getString();

            const void* key_data = query.getColumn(1).getBlob();
            int key_size = query.getColumn(1).getBytes();
            peer.public_key.resize(key_size);
            std::memcpy(peer.public_key.data(), key_data, key_size);

            peer.trust_status = query.getColumn(2).getString();
            peer.first_seen = query.getColumn(3).getInt64();
            peers.push_back(peer);
        }
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to get known peers: " << ex.what() << "\n";
    }

    return peers;
}

std::string DatabaseManager::getFileStoragePath() const {
    const char* home = std::getenv("HOME");
    if (!home) {
        home = "/tmp";
    }
    return std::string(home) + "/.local/share/meshtalk/files/";
}

bool DatabaseManager::saveFileTransfer(const std::string& transfer_id, const std::string& filename,
                                       uint64_t file_size, const std::string& sha256_hash,
                                       const std::string& peer_name, bool is_sender,
                                       const std::string& status, const std::string& timestamp,
                                       int64_t timestamp_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement insert(*db_,
            "INSERT OR REPLACE INTO file_transfers (transfer_id, filename, file_size, sha256_hash, "
            "peer_name, is_sender, status, timestamp, timestamp_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
        insert.bind(1, transfer_id);
        insert.bind(2, filename);
        insert.bind(3, static_cast<int64_t>(file_size));
        insert.bind(4, sha256_hash);
        insert.bind(5, peer_name);
        insert.bind(6, is_sender ? 1 : 0);
        insert.bind(7, status);
        insert.bind(8, timestamp);
        insert.bind(9, timestamp_ms);
        insert.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to save file transfer: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::updateFileTransferStatus(const std::string& transfer_id, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement update(*db_,
            "UPDATE file_transfers SET status = ? WHERE transfer_id = ?");
        update.bind(1, status);
        update.bind(2, transfer_id);
        update.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to update file transfer status: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::saveFileData(const std::string& transfer_id, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement insert(*db_,
            "INSERT OR REPLACE INTO file_data (transfer_id, data) VALUES (?, ?)");
        insert.bind(1, transfer_id);
        insert.bind(2, data.data(), static_cast<int>(data.size()));
        insert.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to save file data: " << ex.what() << "\n";
        return false;
    }
}

std::optional<std::vector<uint8_t>> DatabaseManager::loadFileData(const std::string& transfer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return std::nullopt;
    }

    try {
        SQLite::Statement query(*db_, "SELECT data FROM file_data WHERE transfer_id = ?");
        query.bind(1, transfer_id);

        if (query.executeStep()) {
            const void* data = query.getColumn(0).getBlob();
            int size = query.getColumn(0).getBytes();
            std::vector<uint8_t> result(size);
            std::memcpy(result.data(), data, size);
            return result;
        }
        return std::nullopt;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to load file data: " << ex.what() << "\n";
        return std::nullopt;
    }
}

std::vector<FileTransferRecord> DatabaseManager::loadAllFileTransfers() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<FileTransferRecord> records;

    if (!db_) {
        return records;
    }

    try {
        SQLite::Statement query(*db_,
            "SELECT id, transfer_id, filename, file_size, sha256_hash, peer_name, "
            "is_sender, status, timestamp, timestamp_ms FROM file_transfers "
            "ORDER BY timestamp_ms ASC");

        while (query.executeStep()) {
            FileTransferRecord record;
            record.id = query.getColumn(0).getInt64();
            record.transfer_id = query.getColumn(1).getString();
            record.filename = query.getColumn(2).getString();
            record.file_size = static_cast<uint64_t>(query.getColumn(3).getInt64());
            record.sha256_hash = query.getColumn(4).getString();
            record.peer_name = query.getColumn(5).getString();
            record.is_sender = query.getColumn(6).getInt() != 0;
            record.status = query.getColumn(7).getString();
            record.timestamp = query.getColumn(8).getString();
            record.timestamp_ms = query.getColumn(9).getInt64();
            records.push_back(record);
        }
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to load file transfers: " << ex.what() << "\n";
    }

    return records;
}

std::vector<FileTransferRecord> DatabaseManager::loadFileTransfersForPeer(const std::string& peer_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<FileTransferRecord> records;

    if (!db_) {
        return records;
    }

    try {
        SQLite::Statement query(*db_,
            "SELECT id, transfer_id, filename, file_size, sha256_hash, peer_name, "
            "is_sender, status, timestamp, timestamp_ms FROM file_transfers "
            "WHERE peer_name = ? ORDER BY timestamp_ms ASC");
        query.bind(1, peer_name);

        while (query.executeStep()) {
            FileTransferRecord record;
            record.id = query.getColumn(0).getInt64();
            record.transfer_id = query.getColumn(1).getString();
            record.filename = query.getColumn(2).getString();
            record.file_size = static_cast<uint64_t>(query.getColumn(3).getInt64());
            record.sha256_hash = query.getColumn(4).getString();
            record.peer_name = query.getColumn(5).getString();
            record.is_sender = query.getColumn(6).getInt() != 0;
            record.status = query.getColumn(7).getString();
            record.timestamp = query.getColumn(8).getString();
            record.timestamp_ms = query.getColumn(9).getInt64();
            records.push_back(record);
        }
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to load file transfers for peer: " << ex.what() << "\n";
    }

    return records;
}

std::optional<FileTransferRecord> DatabaseManager::getFileTransfer(const std::string& transfer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return std::nullopt;
    }

    try {
        SQLite::Statement query(*db_,
            "SELECT id, transfer_id, filename, file_size, sha256_hash, peer_name, "
            "is_sender, status, timestamp, timestamp_ms FROM file_transfers "
            "WHERE transfer_id = ?");
        query.bind(1, transfer_id);

        if (query.executeStep()) {
            FileTransferRecord record;
            record.id = query.getColumn(0).getInt64();
            record.transfer_id = query.getColumn(1).getString();
            record.filename = query.getColumn(2).getString();
            record.file_size = static_cast<uint64_t>(query.getColumn(3).getInt64());
            record.sha256_hash = query.getColumn(4).getString();
            record.peer_name = query.getColumn(5).getString();
            record.is_sender = query.getColumn(6).getInt() != 0;
            record.status = query.getColumn(7).getString();
            record.timestamp = query.getColumn(8).getString();
            record.timestamp_ms = query.getColumn(9).getInt64();
            return record;
        }
        return std::nullopt;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to get file transfer: " << ex.what() << "\n";
        return std::nullopt;
    }
}

int DatabaseManager::getUnreadCount(const std::string& peer_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return 0;
    }

    try {
        SQLite::Statement query(*db_, "SELECT count FROM unread_counts WHERE peer_name = ?");
        query.bind(1, peer_name);
        if (query.executeStep()) {
            return query.getColumn(0).getInt();
        }
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to get unread count: " << ex.what() << "\n";
    }
    return 0;
}

bool DatabaseManager::setUnreadCount(const std::string& peer_name, int count) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement insert(*db_,
            "INSERT OR REPLACE INTO unread_counts (peer_name, count) VALUES (?, ?)");
        insert.bind(1, peer_name);
        insert.bind(2, count);
        insert.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to set unread count: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::incrementUnreadCount(const std::string& peer_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement insert(*db_,
            "INSERT INTO unread_counts (peer_name, count) VALUES (?, 1) "
            "ON CONFLICT(peer_name) DO UPDATE SET count = count + 1");
        insert.bind(1, peer_name);
        insert.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to increment unread count: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::clearUnreadCount(const std::string& peer_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement update(*db_,
            "INSERT OR REPLACE INTO unread_counts (peer_name, count) VALUES (?, 0)");
        update.bind(1, peer_name);
        update.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to clear unread count: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::markMessagesAsRead(const std::string& peer_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return false;
    }

    try {
        SQLite::Statement update(*db_,
            "UPDATE messages SET status = 'read' WHERE peer_name = ? AND status = 'unread'");
        update.bind(1, peer_name);
        update.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to mark messages as read: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::saveInfoEntry(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    try {
        SQLite::Statement insert(*db_,
            "INSERT OR REPLACE INTO info_entries (key, value, updated_at) VALUES (?, ?, ?)");
        insert.bind(1, key);
        insert.bind(2, value);
        insert.bind(3, static_cast<int64_t>(std::time(nullptr)));
        insert.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to save info entry: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::deleteInfoEntry(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    try {
        SQLite::Statement del(*db_, "DELETE FROM info_entries WHERE key = ?");
        del.bind(1, key);
        del.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to delete info entry: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::updateInfoEntry(const std::string& key, const std::string& value) {
    return saveInfoEntry(key, value);
}

std::vector<InfoEntry> DatabaseManager::loadAllInfoEntries() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InfoEntry> entries;
    if (!db_) return entries;
    try {
        SQLite::Statement query(*db_, "SELECT key, value, updated_at FROM info_entries ORDER BY key");
        while (query.executeStep()) {
            InfoEntry e;
            e.id = 0;
            e.key = query.getColumn(0).getString();
            e.value = query.getColumn(1).getString();
            e.updated_at = query.getColumn(2).getInt64();
            entries.push_back(e);
        }
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to load info entries: " << ex.what() << "\n";
    }
    return entries;
}

std::optional<std::string> DatabaseManager::getInfoEntry(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    try {
        SQLite::Statement query(*db_, "SELECT value FROM info_entries WHERE key = ?");
        query.bind(1, key);
        if (query.executeStep()) {
            return query.getColumn(0).getString();
        }
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to get info entry: " << ex.what() << "\n";
    }
    return std::nullopt;
}

bool DatabaseManager::savePeerInfo(const std::string& peer_name, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    try {
        SQLite::Statement insert(*db_,
            "INSERT OR REPLACE INTO peer_info (peer_name, key, value, updated_at) VALUES (?, ?, ?, ?)");
        insert.bind(1, peer_name);
        insert.bind(2, key);
        insert.bind(3, value);
        insert.bind(4, static_cast<int64_t>(std::time(nullptr)));
        insert.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to save peer info: " << ex.what() << "\n";
        return false;
    }
}

bool DatabaseManager::clearPeerInfo(const std::string& peer_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    try {
        SQLite::Statement del(*db_, "DELETE FROM peer_info WHERE peer_name = ?");
        del.bind(1, peer_name);
        del.exec();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to clear peer info: " << ex.what() << "\n";
        return false;
    }
}

std::vector<InfoEntry> DatabaseManager::loadPeerInfo(const std::string& peer_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InfoEntry> entries;
    if (!db_) return entries;
    try {
        SQLite::Statement query(*db_,
            "SELECT key, value, updated_at FROM peer_info WHERE peer_name = ? ORDER BY key");
        query.bind(1, peer_name);
        while (query.executeStep()) {
            InfoEntry e;
            e.id = 0;
            e.key = query.getColumn(0).getString();
            e.value = query.getColumn(1).getString();
            e.updated_at = query.getColumn(2).getInt64();
            entries.push_back(e);
        }
    } catch (const std::exception& ex) {
        std::cerr << "db: failed to load peer info: " << ex.what() << "\n";
    }
    return entries;
}
