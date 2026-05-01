# LAN Messenger — Developer Implementation Guide

> A serverless, peer-to-peer terminal messaging system for local networks.  
> **Stack:** C++17 · POSIX Sockets · Protocol Buffers · libsodium · SQLite · ncurses/ftxui · CMake

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Repository Layout](#2-repository-layout)
3. [Build System Setup (CMake)](#3-build-system-setup-cmake)
4. [Proto Schema Definitions](#4-proto-schema-definitions)
5. [Discovery Layer — UDP Broadcast](#5-discovery-layer--udp-broadcast)
6. [Messaging Layer — Direct TCP](#6-messaging-layer--direct-tcp)
7. [Cryptography Layer (v2)](#7-cryptography-layer-v2)
8. [Persistence Layer — SQLite (v2)](#8-persistence-layer--sqlite-v2)
9. [File Transfer Protocol](#9-file-transfer-protocol)
10. [Thread Architecture](#10-thread-architecture)
11. [Terminal UI (TUI)](#11-terminal-ui-tui)
12. [Development Phases & Milestones](#12-development-phases--milestones)
13. [Running & Demo Walkthrough](#13-running--demo-walkthrough)
14. [Known Limitations & Future Work](#14-known-limitations--future-work)

---

## 1. Project Overview

LAN Messenger is a fully serverless P2P chat application. Every peer is equal — there is no central server, no cloud relay, no internet dependency. Communication runs directly over LAN using:

- **UDP broadcast** for zero-config peer discovery
- **Direct TCP** for reliable, ordered message and file delivery
- **Protocol Buffers** for compact binary serialization
- **libsodium** for end-to-end encryption (v2)
- **SQLite** for local message persistence (v2)

### Design Invariants

| Invariant | Mechanism |
|---|---|
| No message leaves the LAN | Pure P2P TCP — no relay |
| No plaintext on the wire (v2) | ChaCha20-Poly1305 per-message encryption |
| No configuration needed | UDP broadcast auto-discovery |
| No internet required | Works on air-gapped networks |
| No single point of failure | Any peer can join or leave at any time |

---

## 2. Repository Layout

```
lan-messenger/
├── CMakeLists.txt
├── proto/
│   ├── discovery.proto         # UDP broadcast messages (HELLO, HEARTBEAT, BYE)
│   └── envelope.proto          # TCP envelope + all payload types
├── src/
│   ├── main.cpp
│   ├── crypto/                 # v2 — encryption layer
│   │   ├── CryptoSession.h/.cpp    # ECDH handshake + session key management
│   │   ├── KeyPair.h/.cpp          # Ed25519 long-term identity keypair
│   │   └── Cipher.h/.cpp           # ChaCha20-Poly1305 encrypt/decrypt wrappers
│   ├── discovery/
│   │   ├── Discovery.h/.cpp        # UDP broadcaster + listener
│   │   └── PeerTable.h/.cpp        # Thread-safe peer registry
│   ├── network/
│   │   ├── TcpServer.h/.cpp        # Accepts incoming TCP connections
│   │   ├── TcpClient.h/.cpp        # Initiates outgoing TCP connections
│   │   └── SocketUtils.h           # POSIX socket helpers (length-prefixed I/O)
│   ├── messaging/
│   │   ├── Session.h/.cpp          # Per-peer session state
│   │   ├── MessageQueue.h          # Thread-safe bounded queue
│   │   └── FileTransfer.h/.cpp     # Chunked file send/receive
│   ├── persistence/            # v2 — SQLite wrapper
│   │   ├── Database.h/.cpp
│   │   └── schema.sql
│   ├── ui/
│   │   ├── TUI.h/.cpp              # Top-level TUI controller
│   │   ├── ChatPane.h/.cpp         # Message rendering
│   │   ├── PeerPane.h/.cpp         # Sidebar peer list
│   │   └── InputBar.h/.cpp         # Command/message input
│   └── utils/
│       ├── Logger.h/.cpp
│       └── UuidGen.h               # UUID v4 generation
└── tests/
```

---

## 3. Build System Setup (CMake)

### Dependencies

Install these before building:

```bash
# Ubuntu / Debian
sudo apt install -y \
  cmake build-essential \
  libprotobuf-dev protobuf-compiler \
  libsodium-dev \
  libsqlite3-dev \
  libncurses-dev    # or libftxui-dev if using ftxui

# macOS (Homebrew)
brew install cmake protobuf libsodium sqlite ncurses
```

### CMakeLists.txt (root)

```cmake
cmake_minimum_required(VERSION 3.20)
project(lan-messenger CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)
pkg_check_modules(SQLITE REQUIRED sqlite3)

# Generate protobuf sources
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS
  proto/discovery.proto
  proto/envelope.proto
)

add_executable(lan-messenger
  src/main.cpp
  src/crypto/CryptoSession.cpp
  src/crypto/KeyPair.cpp
  src/crypto/Cipher.cpp
  src/discovery/Discovery.cpp
  src/discovery/PeerTable.cpp
  src/network/TcpServer.cpp
  src/network/TcpClient.cpp
  src/messaging/Session.cpp
  src/messaging/FileTransfer.cpp
  src/persistence/Database.cpp
  src/ui/TUI.cpp
  src/ui/ChatPane.cpp
  src/ui/PeerPane.cpp
  src/ui/InputBar.cpp
  src/utils/Logger.cpp
  ${PROTO_SRCS}
)

target_include_directories(lan-messenger PRIVATE
  src/
  ${CMAKE_CURRENT_BINARY_DIR}   # for generated proto headers
  ${SODIUM_INCLUDE_DIRS}
  ${SQLITE_INCLUDE_DIRS}
)

target_link_libraries(lan-messenger
  ${Protobuf_LIBRARIES}
  ${SODIUM_LIBRARIES}
  ${SQLITE_LIBRARIES}
  ncurses
  pthread
)
```

### Build & Run

```bash
git clone https://github.com/yourname/lan-messenger
cd lan-messenger
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Launch two instances (same machine or two LAN machines)
./lan-messenger --username alice
./lan-messenger --username bob
```

---

## 4. Proto Schema Definitions

All network messages are serialized using Protocol Buffers. Define two `.proto` files.

### `proto/discovery.proto` — UDP Broadcast

```proto
syntax = "proto3";

message DiscoveryPacket {
  enum Type {
    HELLO     = 0;  // on startup
    HEARTBEAT = 1;  // every 5 seconds
    BYE       = 2;  // on clean shutdown
  }

  Type    type       = 1;
  string  username   = 2;  // display name
  string  ip         = 3;  // sender's LAN IP
  uint32  tcp_port   = 4;  // TCP listening port
  int64   timestamp  = 5;  // Unix epoch ms
  bytes   identity_pk = 6; // Ed25519 public key (v2 only)
}
```

### `proto/envelope.proto` — TCP Messages

```proto
syntax = "proto3";

message ChatMessage {
  string msg_id   = 1;  // UUID v4
  string from     = 2;
  string to       = 3;
  bytes  content  = 4;  // encrypted ciphertext in v2; plaintext in v1
  bytes  nonce    = 5;  // 12-byte ChaCha20 nonce (v2 only)
  int64  timestamp = 6;
}

message FileOffer {
  string file_id   = 1;
  string filename  = 2;
  int64  size      = 3;
  string mime_type = 4;
}

message FileResponse {
  string file_id  = 1;
  bool   accepted = 2;
}

message FileChunk {
  string file_id     = 1;
  uint32 chunk_index = 2;
  bytes  data        = 3;  // up to 64 KB, encrypted in v2
}

message FileComplete {
  string file_id = 1;
  string sha256  = 2;  // integrity check
}

message TypingStatus {
  string from    = 1;
  bool   typing  = 2;
}

message DeliveryAck {
  string msg_id = 1;
}

// Top-level wrapper for all TCP payloads
message Envelope {
  oneof payload {
    ChatMessage  chat       = 1;
    FileOffer    file_offer = 2;
    FileResponse file_resp  = 3;
    FileChunk    file_chunk = 4;
    FileComplete file_done  = 5;
    TypingStatus typing     = 6;
    DeliveryAck  ack        = 7;
  }
}
```

> **Wire format:** All TCP messages are length-prefixed — a 4-byte big-endian uint32 containing the serialized `Envelope` byte count, followed by the payload. This allows clean message framing on the stream.

```cpp
// SocketUtils.h — helpers for length-prefixed I/O
void send_message(int fd, const google::protobuf::Message& msg);
bool recv_message(int fd, google::protobuf::Message& msg);
```

---

## 5. Discovery Layer — UDP Broadcast

### Overview

No configuration. No DNS. Peers find each other automatically within 5 seconds of launch.

| Event | Action |
|---|---|
| Startup | Bind UDP port 55000, send `HELLO` to subnet broadcast address |
| Every 5s | Send `HEARTBEAT` to broadcast address |
| Peer seen | Add/refresh entry in `PeerTable` |
| 3 missed heartbeats (15s) | Mark peer offline, remove from table |
| Clean shutdown | Send `BYE`, all peer tables update immediately |

### `PeerTable` — Data Structure

```cpp
// src/discovery/PeerTable.h

enum class PeerStatus { ONLINE, AWAY, BUSY, OFFLINE };

struct PeerInfo {
  std::string              username;
  std::string              ip;
  uint16_t                 tcp_port;
  PeerStatus               status;
  int64_t                  last_seen_ms;
  std::vector<uint8_t>     identity_pubkey;  // Ed25519 (v2)
};

class PeerTable {
public:
  void   update(const PeerInfo& peer);
  void   remove(const std::string& username);
  bool   isAlive(const std::string& username) const;
  std::vector<PeerInfo> getAll() const;

private:
  std::unordered_map<std::string, PeerInfo> peers_;
  mutable std::shared_mutex                 mutex_;
};
```

### `Discovery` — Broadcaster & Listener

```cpp
// src/discovery/Discovery.h

class Discovery {
public:
  Discovery(std::string username, uint16_t tcp_port, PeerTable& table);

  void start();   // spawns broadcaster + listener threads
  void stop();    // sends BYE, joins threads

private:
  void broadcaster_loop();   // HELLO on start, HEARTBEAT every 5s, BYE on stop
  void listener_loop();      // receives UDP packets, updates PeerTable
  void expire_peers();       // called from listener loop

  int          bcast_fd_;    // UDP send socket
  int          listen_fd_;   // UDP receive socket (bound to port 55000)
  std::string  username_;
  uint16_t     tcp_port_;
  PeerTable&   peer_table_;
  std::atomic<bool> running_{false};
};
```

### Implementation Notes

- Get the broadcast address: read `/proc/net/if_inet6` or use `SIOCGIFBRDADDR` ioctl.
- Set `SO_BROADCAST` on the send socket.
- Set `SO_REUSEADDR` (and `SO_REUSEPORT` on Linux) on the receive socket so multiple processes can bind on the same machine during testing.
- Heartbeat interval: 5 seconds. Expiry threshold: 3 × 5s = 15 seconds.

---

## 6. Messaging Layer — Direct TCP

### Connection Model

On first message to a peer, open a TCP connection to `peer.ip:peer.tcp_port`. Maintain the connection for the session duration. All messages, ACKs, and typing indicators flow over this single connection.

```
Alice (client)              Bob (server)
     |                           |
     |--- TCP connect ---------->|
     |--- ECDH handshake ------->|  (v2 only)
     |<-- ECDH handshake --------|
     |--- Envelope (ChatMessage)->|
     |<-- Envelope (DeliveryAck) |
     |...                        |
```

### `Session` — Per-Peer State

```cpp
// src/messaging/Session.h

class Session {
public:
  Session(std::string peer_username, int sockfd, CryptoSession* crypto);

  void send(const Envelope& env);
  void start_receive_loop(MessageQueue<Envelope>& inbound);
  void close();

  const std::string& peer() const { return peer_username_; }
  bool               is_open() const;

private:
  std::string     peer_username_;
  int             sockfd_;
  CryptoSession*  crypto_;           // null in v1
  std::mutex      send_mutex_;
};
```

### `MessageQueue` — Thread-Safe Bounded Queue

```cpp
// src/messaging/MessageQueue.h

template<typename T>
class MessageQueue {
public:
  explicit MessageQueue(size_t max_size = 512);
  void push(T item);                        // blocks if full
  bool try_push(T item);                    // non-blocking
  T    pop();                               // blocks until item available
  bool try_pop(T& item, int timeout_ms);

private:
  std::queue<T>           queue_;
  std::mutex              mutex_;
  std::condition_variable cv_push_, cv_pop_;
  size_t                  max_size_;
};
```

### TCP Server

```cpp
// src/network/TcpServer.h

class TcpServer {
public:
  explicit TcpServer(uint16_t port);
  void start(std::function<void(int sockfd, std::string peer_ip)> on_accept);
  void stop();

private:
  int  listen_fd_;
  std::atomic<bool> running_;
};
```

---

## 7. Cryptography Layer (v2)

> **Rule:** Never implement crypto primitives yourself. All cryptography goes through libsodium.

### Three-Layer Security Model

| Layer | Algorithm | Purpose |
|---|---|---|
| Identity | Ed25519 | Long-term keypair per user. Broadcast in HELLO, stored to disk. |
| Key Exchange | X25519 (ECDH) | Ephemeral per-session. Derives a shared secret never seen by either party in full. |
| Encryption | ChaCha20-Poly1305 | Encrypts and authenticates every message with the session key + unique nonce. |

> **Critical distinction:** Ed25519 signing alone provides authentication and integrity, but NOT confidentiality. Anyone on the LAN can still read message content if only signing is implemented. True E2E encryption requires ECDH key exchange + symmetric encryption. Both must be present.

### Handshake Flow

```
Alice                                           Bob
  |                                              |
  |-- HELLO {identity_pk_A, ephemeral_pk_A} ---->|
  |<-- HELLO {identity_pk_B, ephemeral_pk_B} ----|
  |                                              |
  |  Both compute:                               |
  |  shared_secret = ECDH(ephemeral_A, ephemeral_B)
  |  session_key   = KDF(shared_secret)          |
  |                                              |
  |-- SIG(handshake_hash, identity_sk_A) ------->|
  |   Bob verifies with identity_pk_A            |
  |<-- SIG(handshake_hash, identity_sk_B) -------|
  |   Alice verifies with identity_pk_B          |
  |                                              |
  |       [Session established]                  |
```

The signed handshake prevents MITM attacks. Without it, an attacker on the LAN could intercept the key exchange and establish two separate encrypted tunnels while reading everything in the middle.

### `KeyPair` — Identity Key Management

```cpp
// src/crypto/KeyPair.h

class KeyPair {
public:
  static KeyPair generate();                  // creates new Ed25519 keypair
  static KeyPair load(const std::string& path); // loads from ~/.config/lan-messenger/id.key
  void save(const std::string& path) const;

  const std::array<uint8_t, 32>& public_key()  const { return pk_; }
  const std::array<uint8_t, 64>& private_key() const { return sk_; }

  std::vector<uint8_t> sign(const uint8_t* data, size_t len) const;

private:
  std::array<uint8_t, 32> pk_;   // Ed25519 public key
  std::array<uint8_t, 64> sk_;   // Ed25519 secret key
};
```

### `CryptoSession` — Per-Peer Session

```cpp
// src/crypto/CryptoSession.h

class CryptoSession {
public:
  // Perform ECDH handshake over an established TCP socket.
  // Returns false if handshake fails (reject the connection).
  bool perform_handshake(int sockfd, const KeyPair& identity, bool is_initiator);

  std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext);
  std::vector<uint8_t> decrypt(const std::vector<uint8_t>& ciphertext);

private:
  std::array<uint8_t, 32> session_key_;
  std::array<uint8_t, 32> local_ephemeral_pk_;
  std::array<uint8_t, 32> local_ephemeral_sk_;
  uint64_t                nonce_counter_{0};   // incremented per message
};
```

### libsodium Implementation

```cpp
#include <sodium.h>

// --- Key Generation ---

// Identity keypair (long-term, persisted to disk)
uint8_t identity_pk[crypto_sign_PUBLICKEYBYTES];
uint8_t identity_sk[crypto_sign_SECRETKEYBYTES];
crypto_sign_keypair(identity_pk, identity_sk);

// Ephemeral keypair (per-session, never persisted)
uint8_t ephemeral_pk[crypto_kx_PUBLICKEYBYTES];
uint8_t ephemeral_sk[crypto_kx_SECRETKEYBYTES];
crypto_kx_keypair(ephemeral_pk, ephemeral_sk);

// --- ECDH Key Exchange ---

uint8_t rx[crypto_kx_SESSIONKEYBYTES], tx[crypto_kx_SESSIONKEYBYTES];

// Initiating side (Alice):
crypto_kx_client_session_keys(rx, tx, client_pk, client_sk, server_pk);

// Accepting side (Bob):
crypto_kx_server_session_keys(rx, tx, server_pk, server_sk, client_pk);

// --- Encrypt a message ---

uint8_t nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES];
randombytes_buf(nonce, sizeof nonce);

crypto_aead_chacha20poly1305_ietf_encrypt(
  ciphertext, &ciphertext_len,
  plaintext,  plaintext_len,
  nullptr, 0,       // no additional data
  nullptr,          // no secret nonce
  nonce, session_key
);

// --- Decrypt a message ---

int result = crypto_aead_chacha20poly1305_ietf_decrypt(
  plaintext, &plaintext_len,
  nullptr,
  ciphertext, ciphertext_len,
  nullptr, 0,
  nonce, session_key
);

if (result != 0) {
  // Authentication failed — reject message, close connection
}

// --- Sign handshake hash ---

uint8_t sig[crypto_sign_BYTES];
crypto_sign_detached(sig, nullptr, handshake_hash, 32, identity_sk);

// --- Verify peer signature ---

if (crypto_sign_verify_detached(sig, handshake_hash, 32, peer_identity_pk) != 0) {
  // Reject connection — MITM or key mismatch
}
```

### Nonce Strategy

Use a monotonically incrementing 64-bit counter per session, padded to 12 bytes. Never reuse a nonce with the same session key. The nonce counter resets to 0 for each new session (new connection = new ephemeral keys = new session key).

```cpp
std::vector<uint8_t> CryptoSession::make_nonce() {
  std::array<uint8_t, 12> nonce{};
  uint64_t n = nonce_counter_++;
  std::memcpy(nonce.data() + 4, &n, 8);  // 4-byte zero pad + 8-byte counter
  return {nonce.begin(), nonce.end()};
}
```

---

## 8. Persistence Layer — SQLite (v2)

The database lives at `~/.config/lan-messenger/history.db`. It is never transmitted over the network.

### Schema

```sql
-- schema.sql

PRAGMA journal_mode = WAL;    -- single writer, multiple readers, no locks on reads
PRAGMA foreign_keys = ON;

-- Known peer identities
CREATE TABLE IF NOT EXISTS peers (
  id              TEXT PRIMARY KEY,   -- UUID
  username        TEXT NOT NULL,
  identity_pubkey BLOB NOT NULL,       -- Ed25519 public key (32 bytes)
  last_seen       INTEGER              -- Unix epoch ms
);

-- Message history (stored as ciphertext)
CREATE TABLE IF NOT EXISTS messages (
  msg_id    TEXT    PRIMARY KEY,       -- UUID v4
  from_peer TEXT    NOT NULL,
  to_peer   TEXT    NOT NULL,
  content   BLOB    NOT NULL,          -- ChaCha20-Poly1305 ciphertext
  nonce     BLOB    NOT NULL,          -- 12-byte nonce
  timestamp INTEGER NOT NULL,          -- Unix epoch ms
  status    TEXT    DEFAULT 'sent'     -- sent | delivered | read
);

-- File transfer history
CREATE TABLE IF NOT EXISTS files (
  file_id   TEXT PRIMARY KEY,
  filename  TEXT NOT NULL,
  path      TEXT NOT NULL,
  size      INTEGER NOT NULL,
  from_peer TEXT NOT NULL,
  status    TEXT DEFAULT 'complete'    -- pending | complete | failed
);

-- Indexes for fast history load
CREATE INDEX IF NOT EXISTS idx_messages_peers ON messages(from_peer, to_peer);
CREATE INDEX IF NOT EXISTS idx_messages_ts    ON messages(timestamp);
```

### `Database` — C++ Wrapper

```cpp
// src/persistence/Database.h

class Database {
public:
  explicit Database(const std::string& path);
  ~Database();

  // Messages
  void   save_message(const ChatMessage& msg);
  std::vector<ChatMessage> load_history(
    const std::string& peer_a,
    const std::string& peer_b,
    int limit = 100
  );
  void   update_status(const std::string& msg_id, const std::string& status);

  // Peers
  void      upsert_peer(const PeerInfo& peer);
  PeerInfo  get_peer(const std::string& username);

  // Files
  void save_file_transfer(const FileTransferRecord& record);

private:
  sqlite3*   db_;
  std::mutex mutex_;    // serialize all writes

  void exec(const std::string& sql);
  void prepare_statements();
};
```

### Design Decisions

| Decision | Rationale |
|---|---|
| Store ciphertext, not plaintext | Session key never written to disk. Compromised device → unreadable history. |
| WAL mode | No read locks during writes. TUI can query history without blocking the sender thread. |
| Single file per user/machine | Simple. No sync needed. No distributed consensus. |
| No cross-device sync (v2) | Out of scope. Planned for v3 using message IDs + timestamps for conflict-free merge. |
| History loaded on chat open | Load the last 100 messages for a peer when the chat window is opened. |

---

## 9. File Transfer Protocol

Files are transferred over the same TCP connection as chat messages using the `FileOffer` / `FileResponse` / `FileChunk` / `FileComplete` protobuf messages.

### Protocol Sequence

```
Sender                              Receiver
  |                                    |
  |-- FileOffer {id, name, size} ----->|
  |                      [UI prompt]   |
  |<-- FileResponse {accepted: true} --|
  |                                    |
  |-- FileChunk {index=0, data} ------>|  (64 KB chunks)
  |-- FileChunk {index=1, data} ------>|
  |-- FileChunk {index=N, data} ------>|
  |                                    |
  |-- FileComplete {sha256} ---------->|
  |                   [verify hash]    |
```

### Implementation

```cpp
// src/messaging/FileTransfer.h

class FileSender {
public:
  FileSender(const std::string& path, Session& session);
  void send(std::function<void(float progress)> on_progress);

private:
  static constexpr size_t CHUNK_SIZE = 64 * 1024;  // 64 KB
  std::string path_;
  Session&    session_;
};

class FileReceiver {
public:
  FileReceiver(const FileOffer& offer, const std::string& download_dir);
  void on_chunk(const FileChunk& chunk);
  bool on_complete(const FileComplete& done);  // returns false if hash mismatch

private:
  std::ofstream            out_;
  std::string              expected_sha256_;
  std::vector<std::string> chunk_hashes_;
};
```

- In v2, each `FileChunk.data` is independently encrypted with the session key before serialization.
- Progress bars on both ends: sender tracks chunks sent, receiver tracks chunks written.
- On hash mismatch at `FileComplete`, receiver deletes the partial file and notifies the user.

---

## 10. Thread Architecture

Every blocking operation runs on its own thread. The TUI thread never blocks on network I/O.

| Thread | Count | Responsibility |
|---|---|---|
| **Main / TUI** | 1 | ncurses/ftxui render loop, keyboard input, pushes to outbound queue |
| **UDP Listener** | 1 | Bound to UDP 55000, receives broadcasts, updates `PeerTable` |
| **UDP Broadcaster** | 1 | HELLO on startup, HEARTBEAT every 5s, BYE on shutdown |
| **TCP Listener** | 1 | `accept()` loop, spawns a Receiver thread per new connection |
| **TCP Receiver** | 1 per peer | Reads length-prefixed envelopes, decrypts, pushes to inbound queue |
| **TCP Sender** | 1 per peer | Pops from outbound queue, encrypts, serializes, writes to socket |
| **Crypto Handshake** | 1 per new conn | ECDH handshake on new connection, signals sender/receiver when ready |
| **File Transfer** | 1 per active transfer | Reads/encrypts/sends chunks (sender) or receives/decrypts/assembles (receiver) |

### Synchronization

- `PeerTable` uses `std::shared_mutex` — multiple readers (TUI, expiry check), exclusive writer (UDP listener).
- `MessageQueue<T>` uses `std::mutex` + `std::condition_variable` for producer/consumer coordination.
- `Database` uses a single `std::mutex` to serialize all SQLite writes (reads are concurrent via WAL).
- `Session::send()` uses a per-session `std::mutex` to prevent concurrent writes to the same socket.

### Startup Sequence

```
main()
  │
  ├─ sodium_init()
  ├─ KeyPair::load() or KeyPair::generate()
  ├─ Database::open()
  ├─ TcpServer::start()         → TCP Listener thread
  ├─ Discovery::start()         → UDP Listener + Broadcaster threads
  ├─ TUI::run()                 → blocks on main thread (render loop)
  │
  └─ [on quit signal]
       Discovery::stop()        → sends BYE, joins threads
       TcpServer::stop()        → closes all sessions
       Database::close()
```

---

## 11. Terminal UI (TUI)

### Layout

```
┌─────────────────────────────────────────────────────────────────────┐
│ ┌──────────────┐ ┌─────────────────────────────────────────────┐  │
│ │   PEERS      │ │  Chat: bob                                  │  │
│ │              │ │                                              │  │
│ │ • alice      │ │  [10:42] alice: hey, can you review PR #42? │  │
│ │ ● bob        │ │  [10:42] bob: sure, sending the diff now    │  │
│ │ ○ charlie(3) │ │  [10:43] bob is typing...                   │  │
│ │              │ │                                              │  │
│ └──────────────┘ └─────────────────────────────────────────────┘  │
│ ┌─────────────────────────────────────────────────────────────────┐ │
│ │ > _                                                             │ │
│ └─────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
```

- **Left pane:** Peer list with online/away/busy indicators and unread message badge.
- **Center pane:** Scrollable chat history for the active peer. Color-coded per peer.
- **Bottom bar:** Command/message input. Prefix with `/` for commands, plain text to send.

### Key Bindings

| Key | Action |
|---|---|
| `Tab` / `Shift+Tab` | Cycle through peer list |
| `Enter` | Send message or execute command |
| `↑` / `↓` | Scroll chat history |
| `PgUp` / `PgDn` | Fast scroll |
| `Ctrl+C` / `/quit` | Broadcast BYE and exit |

### Commands

| Command | Description |
|---|---|
| `/help` | List all commands |
| `/list` | Show all online peers |
| `/chat <username>` | Open chat with peer |
| `/send <filename>` | Send file to current chat peer |
| `/status <online\|away\|busy>` | Change presence status |
| `/clear` | Clear current chat window |
| `/quit` | Broadcast BYE and exit |
| `/room create <name>` | *(v2)* Create a group room |
| `/room join <name>` | *(v2)* Join a group room |
| `/room leave` | *(v2)* Leave current room |

---

## 12. Development Phases & Milestones

| Phase | Milestone | Key Deliverables |
|---|---|---|
| **0** | Project Setup | CMake build, proto schema, logging framework, CI |
| **1A** | Network Core | UDP broadcaster/listener, `PeerTable`, `TcpServer`/`TcpClient`, socket utils |
| **1B** | Messaging | Protobuf serialization, `Session`, `MessageQueue`, 1:1 DM |
| **1C** | Terminal UI | ncurses/ftxui layout, `PeerPane`, `ChatPane`, `InputBar`, colors |
| **1D** | File Transfer | `FileOffer`/`Response`/`Chunk` protocol, progress bar, chunked I/O |
| **1E** | Polish & QA | Delivery ACKs, typing indicators, status changes, unit tests |
| **2A** | E2E Encryption | libsodium integration, ECDH handshake, ChaCha20-Poly1305, signed handshake |
| **2B** | Persistence | SQLite wrapper, schema, encrypted storage, history on reconnect |
| **2C** | Group Chat | Room hosting, member management, host migration, room discovery |

### Testing Strategy

```bash
# Unit tests: run from build/
./tests/test_peer_table
./tests/test_crypto_session
./tests/test_message_queue

# Integration test: two processes on one machine
./lan-messenger --username alice --port 9001 &
./lan-messenger --username bob   --port 9002 &

# Packet inspection: verify ciphertext on wire (v2)
sudo tcpdump -i lo -X port 9001
# Should see only binary noise — no plaintext
```

---

## 13. Running & Demo Walkthrough

```bash
# Terminal 1
./lan-messenger --username alice

# Terminal 2 (same machine or another on the same LAN)
./lan-messenger --username bob
```

1. Both peers appear in each other's sidebar within 5 seconds (3 HEARTBEAT cycles).
2. Alice opens a chat: `/chat bob`
3. On first message, the ECDH handshake completes silently in the background.
4. Messages appear instantly in Bob's window — encrypted in transit, decrypted on receipt.
5. Alice sends a file: `/send report.pdf` — Bob sees a prompt, presses `y` to accept, progress bar appears on both ends.
6. Alice exits (`/quit`) — Bob's sidebar immediately shows Alice as offline (BYE packet received).
7. *(Optional)* Open Wireshark on the LAN interface — captured packets show only binary ciphertext, no plaintext.
8. *(Optional)* Start a third instance and demo `/room create dev` → `/room join dev`.

---

## 14. Known Limitations & Future Work

| Limitation | Details | Planned Fix |
|---|---|---|
| Single subnet only | UDP broadcast doesn't cross routers or VLANs | v3: mDNS or optional unicast peer list |
| Username collisions | Two identical usernames cause undefined `PeerTable` behavior | v2 resolves via UUID as primary key |
| NAT / firewall | Blocked UDP 55000 or TCP listen port breaks discovery or connection | Document firewall rules; add connection error UI |
| Scale ceiling | Heartbeat broadcast creates noise at 100+ peers | v3: gossip protocol for large networks |
| No key revocation | Compromised identity key has no revocation path in v2 | v3: signed revocation broadcast |
| Trust on first use (TOFU) | First connection accepts peer's public key without out-of-band verification | v3: key fingerprint display + manual verification flow |
| No cross-device history | SQLite is local-only per machine | v3: optional P2P sync on reconnect using message IDs + timestamps |
| Session key not persisted | Stored ciphertext cannot be decrypted without the peer's current session | Intentional — safe at rest tradeoff |

---

## Interview-Ready Summary

> *"LAN Messenger is a pure P2P terminal chat application built in C++17. Peer discovery uses UDP broadcast with a 5-second heartbeat — no config, no DNS. Messaging uses direct TCP with length-prefixed protobuf envelopes. In v2, I added end-to-end encryption: X25519 ECDH per-session key exchange, ChaCha20-Poly1305 message encryption, and Ed25519-signed handshakes to prevent MITM attacks. All cryptography goes through libsodium — no custom primitives. Message history is persisted in SQLite as ciphertext alongside the nonce; the session key is never written to disk."*

---

*LAN Messenger v2.0 — Pure P2P | Zero Cloud | E2E Encrypted | LAN Speed*
