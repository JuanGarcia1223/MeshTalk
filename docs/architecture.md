# Architecture

Meshtalk is built as a modular C++ application with clear separation between networking, cryptography, persistence, and UI layers.

## Component Overview

```
+--------------------------------------------------+
|                   TerminalUI                     |
|  (notcurses: people list, chat, input, debug)   |
+--------------------------------------------------+
                         |
         +---------------+---------------+
         |                               |
+--------v--------+          +-----------v-----------+
|  ChatServer     |          | UdpHelloBroadcaster   |
|  (TCP server)   |          |  (UDP discovery)      |
+--------+--------+          +-----------+-----------+
         |                               |
         |  messages / handshakes        |  hello / bye
         |                               |
+--------v--------+          +-----------v-----------+
| SessionManager  |          |      KeyManager       |
| (encrypt/decrypt)|         |  (Ed25519/X25519)     |
+-----------------+          +-----------------------+
         |
+--------v--------+
| DatabaseManager |
|   (SQLite)      |
+-----------------+
```

## Component Responsibilities

### ChatServer
- Binds to an OS-assigned TCP port (accepts inbound connections)
- Spawns an `accept_loop` thread to handle incoming TCP connections
- Maintains an `outbound_fd_by_endpoint_` map for reuse of existing TCP sockets
- Routes messages to the appropriate peer's outbound fd
- Handles file transfer protocol (offers, chunks, completion)
- Sends/receives `Envelope` protobuf messages over TCP

### UdpHelloBroadcaster
- Broadcasts `HELLO` packets via UDP on port 55000 every ~2 seconds
- Listens for `HELLO` and `BYE` packets from other peers
- Calls `on_peer_seen` callback when a peer is discovered
- Calls `on_peer_bye` callback when a peer sends a goodbye
- 10-second timeout window: if no HELLO heard, peer is marked offline

### SessionManager
- Creates ephemeral X25519 keypairs per peer session
- Builds signed handshake payloads (Ed25519 signature)
- Verifies incoming handshake signatures
- Performs ECDH to derive a shared session key
- Encrypts/decrypts envelopes with XSalsa20-Poly1305

### KeyManager
- Loads or generates a persistent Ed25519 identity keypair
- Derives the corresponding X25519 keypair for ECDH
- Provides the public key for discovery broadcasts
- Stores keys in the SQLite database

### DatabaseManager
- SQLite persistence layer
- Stores chat messages, peer identities, file transfers
- Tracks trust status per peer (`trusted` vs `pending`)
- Manages personal info key-value entries
- Each user gets their own database file

### TerminalUI
- Notcurses-based terminal interface
- Three-panel layout: people list, chat window, debug console
- Handles keyboard input, mouse clicks, scrolling
- Manages trust modals, identity popups, file transfer popups
- Draws the command menu and handles slash commands

## Message Flow

### Sending a Message

1. User types a message and presses Enter
2. `TerminalUI` calls `on_send_chat_` callback → `ChatServer::send_chat`
3. `ChatServer` looks up or creates an outbound TCP connection
4. `SessionManager::encrypt` wraps the plaintext in an `Envelope`
5. `send_envelope` serializes and sends the encrypted envelope
6. `TerminalUI` shows the message as "S: ..." (pending if ACK not yet received)

### Receiving a Message

1. `accept_loop` accepts a TCP connection
2. `handle_inbound_connection` reads the encrypted envelope
3. `SessionManager::decrypt` unwraps the ciphertext
4. `handle_chat_message` parses the `ChatMessage` payload
5. A `DELIVERY_ACK` envelope is sent back on the same fd
6. `on_receive_` callback adds the message to `TerminalUI`

### Handshake Flow

1. Initiator calls `connect_to` when session is not ready
2. `SessionManager::initSession` generates an ephemeral X25519 keypair
3. `buildHandshakePayload` creates an `Envelope::HANDSHAKE` with Ed25519 pk, X25519 pk, and signature
4. Responder receives the handshake in `handle_inbound_connection`
5. Responder verifies signature, performs ECDH, derives session key
6. Responder sends back a handshake response
7. Initiator's outbound reader thread receives the response and completes the session

## Thread Model

| Thread | Source | Purpose |
|--------|--------|---------|
| `accept_thread_` | ChatServer | Accepts inbound TCP connections |
| `broadcaster_thread_` | UdpHelloBroadcaster | Sends UDP HELLO packets |
| `receiver_thread_` | UdpHelloBroadcaster | Receives UDP HELLO/BYE packets |
| `timeout_checker_thread_` | TerminalUI | Marks peers offline after timeout |
| Outbound reader threads | ChatServer::connect_to | Continuously read ACKs/handshakes on outbound fds |
| Inbound handler threads | ChatServer::accept_loop | Handle each accepted connection |
| stdout/stderr capture | TerminalUI | Capture stdio for debug panel |
