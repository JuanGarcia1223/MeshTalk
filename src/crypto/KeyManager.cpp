#include "crypto/KeyManager.h"

#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

KeyManager::KeyManager(DatabaseManager& db) : db_(db) {}

bool KeyManager::init() {
    return loadOrCreateKeys();
}

bool KeyManager::generateKeypair() {
    // Generate random keys (mock implementation - replace with libsodium later)
    public_key_.resize(PUBLIC_KEY_SIZE);
    private_key_.resize(PRIVATE_KEY_SIZE);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    for (size_t i = 0; i < PUBLIC_KEY_SIZE; ++i) {
        public_key_[i] = static_cast<uint8_t>(dis(gen));
    }
    for (size_t i = 0; i < PRIVATE_KEY_SIZE; ++i) {
        private_key_[i] = static_cast<uint8_t>(dis(gen));
    }

    // Save to database
    if (!db_.saveIdentity(public_key_, private_key_)) {
        std::cerr << "crypto: failed to save identity to database\n";
        return false;
    }

    std::cout << "crypto: generated new keypair, fingerprint: " << myFingerprint() << "\n";
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
