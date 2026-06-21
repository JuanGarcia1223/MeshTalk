#pragma once

#include "db/DatabaseManager.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class KeyManager {
public:
    static constexpr size_t PUBLIC_KEY_SIZE = 32;   // libsodium crypto_sign_PUBLICKEYBYTES
    static constexpr size_t PRIVATE_KEY_SIZE = 64;  // libsodium crypto_sign_SECRETKEYBYTES

    explicit KeyManager(DatabaseManager& db);
    ~KeyManager() = default;

    KeyManager(const KeyManager&) = delete;
    KeyManager& operator=(const KeyManager&) = delete;

    // Initialize - generates or loads keypair
    bool init();

    // Getters
    const std::vector<uint8_t>& publicKey() const { return public_key_; }
    const std::vector<uint8_t>& privateKey() const { return private_key_; }
    const std::vector<uint8_t>& x25519PublicKey() const { return x25519_public_key_; }
    const std::vector<uint8_t>& x25519SecretKey() const { return x25519_secret_key_; }

    // Format public key as fingerprint (first 16 bytes as 4 groups of 4 hex chars)
    static std::string fingerprint(const std::vector<uint8_t>& public_key);
    std::string myFingerprint() const;

    // Check if initialized
    bool hasKeys() const { return !public_key_.empty() && !private_key_.empty(); }

    void set_logger(std::function<void(const std::string&)> fn) { logger_ = std::move(fn); }

private:
    bool generateKeypair();
    bool loadOrCreateKeys();
    bool deriveX25519Keys();
    void log(const std::string& msg) const;

    DatabaseManager& db_;
    std::vector<uint8_t> public_key_;
    std::vector<uint8_t> private_key_;
    std::vector<uint8_t> x25519_public_key_;
    std::vector<uint8_t> x25519_secret_key_;
    std::function<void(const std::string&)> logger_;
};
