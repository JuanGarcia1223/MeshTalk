# Meshtalk Documentation

Meshtalk is a peer-to-peer encrypted chat application designed for local network mesh communication. It features end-to-end encryption, automatic peer discovery, file transfer, and a terminal-based user interface.

## Table of Contents

- [Architecture](architecture.md) — System design and component overview
- [Cryptography](crypto.md) — Key management, sessions, and trust model
- [Networking](networking.md) — UDP discovery, TCP connections, and protocol
- [User Interface](ui.md) — Terminal UI layout, navigation, and interactions
- [Command-Line Interface](cli.md) — Startup flags and slash commands
- [File Transfer](file-transfer.md) — Sending and receiving files
- [Identity System](identity.md) — Personal info, fingerprints, and verification

## Quick Start

```bash
# Build
mkdir build && cd build
cmake .. && make -j$(nproc)

# Run
./meshtalk_demo --name alice

# With debug output
./meshtalk_demo --name alice --debug UDPB,CRYPTO

# Enable pending-ACK visual indicator
./meshtalk_demo --name alice --enable-ack-show
```

## Core Features

- **End-to-end encryption** — Each peer pair establishes a unique session key via X25519 ECDH, with Ed25519 signatures for authentication
- **Automatic discovery** — UDP broadcast on port 55000 finds peers on the same LAN without configuration
- **Persistent trust** — Once you trust a peer, they remain trusted across restarts until their identity key changes
- **File transfer** — Send files directly to peers with progress tracking
- **Offline messaging** — Messages are sent when the peer comes back online
- **Delivery acknowledgments** — Know when your message has been received

## Project Structure

```
src/
  main.cpp                 — Entry point, object wiring
  chat/ChatServer.{h,cpp}  — TCP server, message routing, file transfer
  crypto/KeyManager.{h,cpp}     — Ed25519/X25519 identity keypair
  crypto/SessionManager.{h,cpp} — Per-peer ephemeral session keys
  db/DatabaseManager.{h,cpp}    — SQLite persistence (messages, peers, identity)
  discovery/UdpHelloBroadcaster.{h,cpp} — UDP peer discovery
  ui/TerminalUI.{h,cpp}    — Notcurses-based terminal interface
  proto/*.proto            — Protocol Buffers message definitions
```

## Database

Each user gets a separate SQLite database at `~/.meshtalk/<username>.db` containing:

- `chat_messages` — All sent and received messages
- `known_peers` — Peer identities with trust status
- `identity` — Your own Ed25519 keypair
- `file_transfers` — File transfer metadata
- `info_entries` — Your personal info key-value pairs
- `peer_info` — Cached info received from other peers
