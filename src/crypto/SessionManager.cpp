#include "crypto/SessionManager.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <sodium.h>

SessionManager::SessionManager(KeyManager& key_manager, DatabaseManager& db)
    : key_manager_(key_manager), db_(db) {}

SessionState* SessionManager::getSession(const std::string& peer_name) {
    auto it = sessions_.find(peer_name);
    return (it != sessions_.end()) ? &it->second : nullptr;
}

const SessionState* SessionManager::getSession(const std::string& peer_name) const {
    auto it = sessions_.find(peer_name);
    return (it != sessions_.end()) ? &it->second : nullptr;
}

void SessionManager::log(const std::string& msg) const {
    if (logger_) logger_(msg);
}

std::string SessionManager::hex_prefix(const std::vector<uint8_t>& data, size_t n) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < std::min(n, data.size()); ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

void SessionManager::initSession(const std::string& peer_name, bool we_initiated) {
    SessionState session;
    session.our_ephemeral_x25519_pk.resize(crypto_scalarmult_curve25519_BYTES);
    session.our_ephemeral_x25519_sk.resize(crypto_scalarmult_curve25519_BYTES);
    crypto_box_keypair(session.our_ephemeral_x25519_pk.data(),
                       session.our_ephemeral_x25519_sk.data());
    session.we_initiated = we_initiated;
    std::string pk_hex = hex_prefix(session.our_ephemeral_x25519_pk, 8);
    sessions_[peer_name] = std::move(session);
    log("Generated ephemeral X25519 keypair for " + peer_name +
        " pk=" + pk_hex);
}

std::vector<uint8_t> SessionManager::buildHandshakePayload(const std::string& peer_name) {
    SessionState* session = getSession(peer_name);
    if (!session) {
        return {};
    }

    // Build HandshakePayload proto manually as raw bytes
    // ed25519_pk (field 1, bytes), x25519_pk (field 2, bytes), signature (field 3, bytes)
    const auto& ed_pk = key_manager_.publicKey();
    const auto& x_pk = session->our_ephemeral_x25519_pk;

    // Sign the ephemeral X25519 public key with our Ed25519 secret key
    std::vector<uint8_t> sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr,
                         x_pk.data(), x_pk.size(),
                         key_manager_.privateKey().data());

    // Serialize as protobuf wire format manually
    auto write_varint = [](std::vector<uint8_t>& out, uint64_t val) {
        while (val >= 0x80) {
            out.push_back(static_cast<uint8_t>(val | 0x80));
            val >>= 7;
        }
        out.push_back(static_cast<uint8_t>(val));
    };

    auto write_field = [&write_varint](std::vector<uint8_t>& out, int field_num,
                                       int wire_type, const uint8_t* data, size_t len) {
        uint64_t tag = (static_cast<uint64_t>(field_num) << 3) | static_cast<uint64_t>(wire_type);
        write_varint(out, tag);
        write_varint(out, len);
        out.insert(out.end(), data, data + len);
    };

    std::vector<uint8_t> payload;
    write_field(payload, 1, 2, ed_pk.data(), ed_pk.size());
    write_field(payload, 2, 2, x_pk.data(), x_pk.size());
    write_field(payload, 3, 2, sig.data(), sig.size());

    log("Handshake payload for " + peer_name + 
        " ed25519_pk=" + hex_prefix(ed_pk, 8) +
        " x25519_pk=" + hex_prefix(x_pk, 8) +
        " sig=" + hex_prefix(sig, 8));
    return payload;
}

bool SessionManager::processHandshake(const std::string& peer_name,
                                      const std::vector<uint8_t>& their_ed25519_pk,
                                      const std::vector<uint8_t>& their_x25519_pk,
                                      const std::vector<uint8_t>& signature) {
    SessionState* session = getSession(peer_name);
    if (!session) {
        std::cerr << "session: no session for peer " << peer_name << "\n";
        return false;
    }

    // 1. Verify the signature against their stored trusted Ed25519 public key
    if (their_ed25519_pk.size() != crypto_sign_PUBLICKEYBYTES) {
        std::cerr << "session: invalid ed25519 pk size from " << peer_name << "\n";
        return false;
    }
    if (their_x25519_pk.size() != crypto_scalarmult_curve25519_BYTES) {
        std::cerr << "session: invalid x25519 pk size from " << peer_name << "\n";
        return false;
    }
    if (signature.size() != crypto_sign_BYTES) {
        std::cerr << "session: invalid signature size from " << peer_name << "\n";
        return false;
    }

    log("Verifying handshake signature from " + peer_name +
        " their_ed25519=" + hex_prefix(their_ed25519_pk, 8) +
        " their_x25519=" + hex_prefix(their_x25519_pk, 8));

    int ok = crypto_sign_verify_detached(
        signature.data(),
        their_x25519_pk.data(), their_x25519_pk.size(),
        their_ed25519_pk.data());

    if (ok != 0) {
        std::cerr << "session: signature verification FAILED for " << peer_name << "\n";
        log("Signature verification FAILED for " + peer_name);
        return false;
    }
    log("Signature valid for " + peer_name);

    // 2. ECDH: our ephemeral secret * their ephemeral public
    std::vector<uint8_t> shared_secret(crypto_scalarmult_BYTES);
    if (crypto_scalarmult(shared_secret.data(),
                          session->our_ephemeral_x25519_sk.data(),
                          their_x25519_pk.data()) != 0) {
        std::cerr << "session: ECDH failed for " << peer_name << "\n";
        log("ECDH failed for " + peer_name);
        return false;
    }
    log("ECDH complete for " + peer_name);

    // 3. Derive session key using both ephemeral public keys as context
    uint8_t context[64];
    const auto& our_pk = session->our_ephemeral_x25519_pk;
    if (memcmp(our_pk.data(), their_x25519_pk.data(), 32) < 0) {
        std::memcpy(context, our_pk.data(), 32);
        std::memcpy(context + 32, their_x25519_pk.data(), 32);
    } else {
        std::memcpy(context, their_x25519_pk.data(), 32);
        std::memcpy(context + 32, our_pk.data(), 32);
    }

    session->session_key.resize(crypto_aead_chacha20poly1305_IETF_KEYBYTES);
    crypto_generichash(session->session_key.data(), session->session_key.size(),
                       shared_secret.data(), shared_secret.size(),
                       context, sizeof(context));

    // Clear shared secret from memory
    sodium_memzero(shared_secret.data(), shared_secret.size());

    session->handshake_complete = true;
    log("Session key derived for " + peer_name + " key=" + hex_prefix(session->session_key, 8));
    std::cout << "session: handshake complete with " << peer_name << "\n";
    return true;
}

std::optional<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>
SessionManager::encrypt(const std::string& peer_name, const std::vector<uint8_t>& plaintext) {
    SessionState* session = getSession(peer_name);
    if (!session || !session->handshake_complete) {
        return std::nullopt;
    }

    uint8_t nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES] = {};
    uint64_t counter = session->send_nonce_counter++;
    std::memcpy(nonce, &counter, sizeof(counter));

    std::vector<uint8_t> ciphertext(plaintext.size() + crypto_aead_chacha20poly1305_IETF_ABYTES);
    crypto_aead_chacha20poly1305_ietf_encrypt(
        ciphertext.data(), nullptr,
        plaintext.data(), plaintext.size(),
        nullptr, 0,
        nullptr,
        nonce, session->session_key.data());

    std::string hex;
    for (size_t i = 0; i < ciphertext.size() && i < 64; ++i) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned int>(ciphertext[i]));
        hex += buf;
    }
    if (ciphertext.size() > 64) hex += "...";
    log("Encrypt for " + peer_name + " plaintext=" + std::to_string(plaintext.size()) +
        " counter=" + std::to_string(counter) + " ciphertext=" + std::to_string(ciphertext.size()) +
        " hex=" + hex);
    return std::make_pair(std::move(ciphertext),
                          std::vector<uint8_t>(nonce, nonce + sizeof(nonce)));
}

std::optional<std::vector<uint8_t>>
SessionManager::decrypt(const std::string& peer_name,
                        const std::vector<uint8_t>& ciphertext,
                        const std::vector<uint8_t>& nonce) {
    SessionState* session = getSession(peer_name);
    if (!session || !session->handshake_complete) {
        return std::nullopt;
    }

    if (nonce.size() != crypto_aead_chacha20poly1305_IETF_NPUBBYTES) {
        return std::nullopt;
    }

    std::vector<uint8_t> plaintext(ciphertext.size() - crypto_aead_chacha20poly1305_IETF_ABYTES);
    int result = crypto_aead_chacha20poly1305_ietf_decrypt(
        plaintext.data(), nullptr,
        nullptr,
        ciphertext.data(), ciphertext.size(),
        nullptr, 0,
        nonce.data(), session->session_key.data());

    if (result != 0) {
        log("Decrypt FAILED for " + peer_name + " ciphertext=" + std::to_string(ciphertext.size()) +
            " nonce=" + hex_prefix(nonce, 8));
        return std::nullopt;
    }
    log("Decrypt for " + peer_name + " ciphertext=" + std::to_string(ciphertext.size()) +
        " nonce=" + hex_prefix(nonce, 8) + " plaintext=" + std::to_string(plaintext.size()));
    return plaintext;
}

bool SessionManager::isReady(const std::string& peer_name) const {
    const SessionState* session = getSession(peer_name);
    return session && session->handshake_complete;
}

bool SessionManager::weInitiated(const std::string& peer_name) const {
    const SessionState* session = getSession(peer_name);
    return session && session->we_initiated;
}

bool SessionManager::hasSession(const std::string& peer_name) const {
    return sessions_.count(peer_name) > 0;
}

void SessionManager::aliasSession(const std::string& from_key, const std::string& to_key) {
    auto it = sessions_.find(from_key);
    if (it != sessions_.end()) {
        sessions_[to_key] = it->second;
        log("Aliased session from " + from_key + " to " + to_key);
    }
}

void SessionManager::clearSession(const std::string& peer_name) {
    auto it = sessions_.find(peer_name);
    if (it != sessions_.end()) {
        // Securely wipe ephemeral secret key before erasing
        if (!it->second.our_ephemeral_x25519_sk.empty()) {
            sodium_memzero(it->second.our_ephemeral_x25519_sk.data(),
                           it->second.our_ephemeral_x25519_sk.size());
        }
        sessions_.erase(it);
        log("Cleared session for " + peer_name);
    }
}
