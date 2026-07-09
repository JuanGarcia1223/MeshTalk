# Cryptography

Meshtalk uses libsodium for all cryptographic operations. The design prioritizes forward secrecy via ephemeral session keys while maintaining persistent identity for authentication.

## Key Hierarchy

### Identity Keypair (Persistent)

- **Algorithm**: Ed25519 (signing)
- **Storage**: SQLite database (`identity` table)
- **Purpose**: Long-term identity, signature verification, fingerprint generation
- **Lifetime**: Permanent unless the database is deleted

The Ed25519 public key is broadcast in UDP discovery packets so peers can identify you before any TCP connection is established.

### X25519 Keypair (Derived)

- **Algorithm**: X25519 (ECDH)
- **Derivation**: Converted from the Ed25519 keypair via `crypto_sign_ed25519_pk_to_curve25519`
- **Purpose**: Used in the handshake to derive the shared session key
- **Lifetime**: Same as identity keypair

### Ephemeral Session Keypair (Per Session)

- **Algorithm**: X25519
- **Generation**: `crypto_box_keypair()` — a fresh keypair for every peer session
- **Purpose**: Provides forward secrecy
- **Lifetime**: One session (in-memory only, lost on restart)

## Handshake Protocol

```
Alice                           Bob
  |                               |
  |-- HANDSHAKE ----------------->|
  |  ed25519_pk                   |
  |  x25519_pk (ephemeral)        |
  |  signature                    |
  |                               |
  |<-- HANDSHAKE RESPONSE --------|
  |  ed25519_pk                   |
  |  x25519_pk (ephemeral)        |
  |  signature                    |
  |                               |
  |  ECDH: shared secret derived  |
  |  Session key = SHA256(secret) |
```

### Handshake Envelope

```protobuf
message Handshake {
  bytes ed25519_pk = 1;   // 32 bytes
  bytes x25519_pk  = 2;   // 32 bytes
  bytes signature  = 3;   // 64 bytes (Ed25519 sig of x25519_pk)
}
```

### Signature Verification

The signature covers the ephemeral X25519 public key. This proves ownership of the Ed25519 identity key without revealing the private key. If signature verification fails, the handshake is rejected and the session is not established.

### Session Key Derivation

After ECDH, the shared secret is hashed with SHA-256 to produce the 32-byte session key used for XSalsa20-Poly1305 encryption.

## Encryption

### Envelope Format

All messages are wrapped in an outer `Envelope` that carries the ciphertext:

```protobuf
message Envelope {
  Type type = 1;          // CHAT, HANDSHAKE, FILE_OFFER, etc.
  bytes nonce = 2;        // 24 bytes for XSalsa20-Poly1305
  bytes ciphertext = 3;   // Encrypted inner protobuf
}
```

The inner payload (e.g., `ChatMessage`) is serialized to bytes, encrypted, and placed in `ciphertext`. The `nonce` is a 24-byte random value. The `type` field on the outer envelope is always `CHAT` for encrypted payloads (a legacy artifact).

### Encryption Flow

1. Serialize the inner message (e.g., `ChatMessage`) to a byte array
2. Generate a random 24-byte nonce
3. Encrypt with `crypto_secretbox_easy` using the session key
4. Build the outer `Envelope` with the nonce and ciphertext
5. Send the serialized `Envelope`

### Decryption Flow

1. Receive the outer `Envelope`
2. Extract nonce and ciphertext
3. Decrypt with `crypto_secretbox_open_easy` using the session key
4. Parse the decrypted bytes into the inner message type

## Trust Model

### Fingerprint

The first 16 bytes of the Ed25519 public key are formatted as 4 groups of 4 hex characters:

```
B330B60C:1F4D7932:5E934DA8:54D44449
```

This is the human-readable identity you verify out-of-band.

### Trust States

| State | Meaning | UI Indicator |
|-------|---------|-------------|
| `trusted` | You accepted this fingerprint | No warning, green dot when online |
| `pending` | New peer, not yet trusted | Yellow dot, trust modal shown |
| `mismatch` | Known peer with different key | Red dot, key mismatch warning |

### Trust Persistence

Trusted peers are stored in the SQLite database with `trust_status = "trusted"`. On startup, the UI loads all trusted peers from the database and marks them as trusted immediately, even before UDP discovery brings them online. Only a key mismatch (different Ed25519 public key for the same username) will move a trusted peer to the `mismatch` state.

### Key Mismatch Handling

If a previously trusted peer connects with a different Ed25519 public key:
1. The new key is detected during UDP discovery or handshake
2. A key mismatch modal is shown with both fingerprints
3. The peer is moved to the `mismatch` state
4. Messaging is blocked until the user explicitly re-trusts

## Security Properties

- **Authentication**: Ed25519 signatures prove identity
- **Confidentiality**: XSalsa20-Poly1305 encrypts all messages
- **Forward secrecy**: Fresh X25519 ephemeral keys per session
- **Integrity**: Poly1305 MAC on every message
- **No replay protection**: Nonces are monotonic counters per session, not timestamps

## Known Limitations

- Session keys are ephemeral (in-memory only). A restart requires a fresh handshake.
- No certificate authority or web-of-trust; out-of-band fingerprint verification is required.
- The outer envelope `type` field does not reliably indicate the inner message type after encryption.
