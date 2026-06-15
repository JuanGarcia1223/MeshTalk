#include "crypto/KeyManager.h"

#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

#include <sodium.h>

KeyManager::KeyManager(DatabaseManager& db) : db_(db) {}

bool KeyManager::init() {
    if (sodium_init() < 0) {
        std::cerr << "crypto: libsodium initialization failed\n";
        return false;
    }
    return loadOrCreateKeys();
}

bool KeyManager::generateKeypair() {
    public_key_.resize(crypto_sign_PUBLICKEYBYTES);
    private_key_.resize(crypto_sign_SECRETKEYBYTES);

    crypto_sign_keypair(public_key_.data(), private_key_.data());

    if (!db_.saveIdentity(public_key_, private_key_)) {
        std::cerr << "crypto: failed to save identity to database\n";
        return false;
    }

    std::cout << "crypto: generated Ed25519 keypair, fingerprint: " << myFingerprint() << "\n";
    return true;
}

bool KeyManager::loadOrCreateKeys() {
    // Try to load existing keys
    if (db_.loadIdentity(public_key_, private_key_)) {
        if (public_key_.size() == PUBLIC_KEY_SIZE && private_key_.size() == PRIVATE_KEY_SIZE) {
            std::cout << "crypto: loaded keypair, fingerprint: " << myFingerprint() << "\n";
            return true;
        }
        std::cerr << "crypto: loaded keys have wrong size, regenerating\n";
        public_key_.clear();
        private_key_.clear();
    }

    // Generate new keys
    return generateKeypair();
}

std::string KeyManager::fingerprint(const std::vector<uint8_t>& public_key) {
    if (public_key.size() < 16) {
        return "INVALID";
    }

    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');

    for (size_t i = 0; i < 16; ++i) {
        if (i > 0 && i % 4 == 0) {
            oss << ":";
        }
        oss << std::setw(2) << static_cast<int>(public_key[i]);
    }

    return oss.str();
}

std::string KeyManager::myFingerprint() const {
    return fingerprint(public_key_);
}
