#include "db/SqliteTest.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdio>
#include <iostream>
#include <string>

bool SqliteTest::test() {
    constexpr const char* kDbPath = "sqlite_test.db";
    std::remove(kDbPath);

    try {
        SQLite::Database db(kDbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

        db.exec("CREATE TABLE messages ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "sender TEXT NOT NULL,"
                "body TEXT NOT NULL,"
                "created_at TEXT NOT NULL"
                ");");

        SQLite::Statement insert(
            db, "INSERT INTO messages(sender, body, created_at) VALUES (?, ?, ?)");
        insert.bind(1, "alice");
        insert.bind(2, "hello world");
        insert.bind(3, "2026-05-14 12:00");
        insert.exec();

        SQLite::Statement count_stmt(db, "SELECT COUNT(*) FROM messages");
        int count = 0;
        if (count_stmt.executeStep()) {
            count = count_stmt.getColumn(0).getInt();
        }
        std::cout << "sqlite: row count after insert = " << count << "\n";

        SQLite::Statement update_stmt(
            db, "UPDATE messages SET body = ? WHERE sender = ?");
        update_stmt.bind(1, "updated hello");
        update_stmt.bind(2, "alice");
        update_stmt.exec();
        std::cout << "sqlite: rows updated = " << db.getChanges() << "\n";

        SQLite::Statement read_stmt(
            db, "SELECT id, sender, body, created_at FROM messages ORDER BY id");
        while (read_stmt.executeStep()) {
            const int id = read_stmt.getColumn(0).getInt();
            const std::string sender = read_stmt.getColumn(1).getString();
            const std::string body = read_stmt.getColumn(2).getString();
            const std::string created = read_stmt.getColumn(3).getString();
            std::cout << "sqlite: row id=" << id << " sender=" << sender
                      << " body=\"" << body << "\" created_at=" << created << "\n";
        }

        {
            SQLite::Transaction tx(db);
            SQLite::Statement tx_insert(
                db, "INSERT INTO messages(sender, body, created_at) VALUES (?, ?, ?)");
            tx_insert.bind(1, "bob");
            tx_insert.bind(2, "inside tx");
            tx_insert.bind(3, "2026-05-14 12:05");
            tx_insert.exec();
            tx.commit();
        }
        std::cout << "sqlite: rows after transaction commit = " << db.execAndGet("SELECT COUNT(*) FROM messages").getInt() << "\n";

        SQLite::Statement delete_stmt(db, "DELETE FROM messages WHERE sender = ?");
        delete_stmt.bind(1, "alice");
        delete_stmt.exec();
        std::cout << "sqlite: rows deleted = " << db.getChanges() << "\n";

        std::cout << "sqlite: final row count = "
                  << db.execAndGet("SELECT COUNT(*) FROM messages").getInt() << "\n";
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "sqlite test failed: " << ex.what() << "\n";
        return false;
    }
}

