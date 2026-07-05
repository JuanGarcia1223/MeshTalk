#pragma once

#include "crypto/KeyManager.h"
#include "db/DatabaseManager.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct SessionState {
    std::vector<uint8_t> our_ephemeral_x25519_pk;
    std::vector<uint8_t> our_ephemeral_x25519_sk;
    std::vector<uint8_t> session_key;
    bool handshake_complete = false;
    bool we_initiated = false;
    uint64_t send_nonce_counter = 0;
};

class SessionManager {
public:
    SessionManager(KeyManager& key_manager, DatabaseManager& db);
    ~SessionManager() = default;

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // Initiate a new session for a peer (generates ephemeral X25519 keypair)
    void initSession(const std::string& peer_name, bool we_initiated);

    // Build handshake payload bytes signed with our Ed25519 key
    std::vector<uint8_t> buildHandshakePayload(const std::string& peer_name);

    // Process a received handshake: verify signature, do ECDH, derive session key
    bool processHandshake(const std::string& peer_name,
                          const std::vector<uint8_t>& their_ed25519_pk,
                          const std::vector<uint8_t>& their_x25519_pk,
                          const std::vector<uint8_t>& signature);

    // Encrypt plaintext for a peer
    std::optional<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>
    encrypt(const std::string& peer_name, const std::vector<uint8_t>& plaintext);

    // Decrypt ciphertext from a peer
    std::optional<std::vector<uint8_t>>
    decrypt(const std::string& peer_name,
            const std::vector<uint8_t>& ciphertext,
            const std::vector<uint8_t>& nonce);

    bool isReady(const std::string& peer_name) const;
    bool weInitiated(const std::string& peer_name) const;
    bool hasSession(const std::string& peer_name) const;

    // Alias an existing session so it can be looked up by a different key
    // (e.g. when peer name is discovered after session was keyed by IP)
    void aliasSession(const std::string& from_key, const std::string& to_key);

    void set_logger(std::function<void(const std::string&)> fn) { logger_ = std::move(fn); }

private:
    SessionState* getSession(const std::string& peer_name);
    const SessionState* getSession(const std::string& peer_name) const;
    void log(const std::string& msg) const;
    static std::string hex_prefix(const std::vector<uint8_t>& data, size_t n);

    KeyManager& key_manager_;
    DatabaseManager& db_;
    std::unordered_map<std::string, SessionState> sessions_;
    std::function<void(const std::string&)> logger_;
};
