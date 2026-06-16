#pragma once

#include "crypto/KeyManager.h"
#include "db/DatabaseManager.h"

#include <cstdint>
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

private:
    SessionState* getSession(const std::string& peer_name);
    const SessionState* getSession(const std::string& peer_name) const;

    KeyManager& key_manager_;
    DatabaseManager& db_;
    std::unordered_map<std::string, SessionState> sessions_;
};
