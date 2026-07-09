# Networking

Meshtalk uses a dual-socket approach: UDP for peer discovery and TCP for message/file transfer.

## UDP Discovery

### Port
- Fixed UDP port **55000** for discovery broadcast/receive
- Each peer binds both a send and receive socket on this port

### Discovery Packet (Protobuf)

```protobuf
message DiscoveryPacket {
  enum Type {
    HELLO = 0;
    BYE   = 1;
  }
  Type type = 1;
  string username = 2;
  uint32 tcp_port = 3;
  string payload_ip = 4;
  bytes identity_pk = 5;  // Ed25519 public key (32 bytes)
}
```

### HELLO Broadcast

Sent every ~2 seconds to the broadcast address (`255.255.255.255`):
- Announces your username, TCP port, and Ed25519 public key
- Received by all peers on the LAN
- Triggers `on_peer_seen` callback

### BYE Packet

Sent when the application shuts down (Ctrl+C):
- Immediately marks the peer as offline on all receivers
- Triggers `on_peer_bye` callback

### Peer Timeout

If no HELLO is received from a peer for **10 seconds**:
- `TerminalUI::run_timeout_checker` marks them offline
- Outbound TCP connection is closed via `disconnect_peer`
- The peer remains in the people list but with a gray dot

## TCP Connections

### Port Assignment

The `ChatServer` binds to port 0 (OS-assigned) and queries the actual port. This port is then advertised in UDP HELLO packets.

### Connection Model

Meshtalk uses **two TCP connections per peer pair**:

1. **Inbound connection** (A → B): B's `accept_loop` accepts, spawns a handler thread
2. **Outbound connection** (B → A): B's `connect_to` opens a socket to A

Each direction has its own fd. This simplifies reasoning about who reads from which socket.

### Connection Lifecycle

```
Peer A                          Peer B
  |                               |
  |<-- inbound fd (A accepts) ----|
  |                               |
  |-- outbound fd (B connects) -->|
  |                               |
  |  Both fds are persistent      |
  |  until peer goes offline      |
```

### Outbound Reader Thread

Every outbound fd gets a dedicated detached thread that:
1. If a handshake is needed: reads the handshake response with a 5-second timeout
2. Enters a continuous read loop processing:
   - `DELIVERY_ACK` — decrypts and calls `handle_delivery_ack`
   - `HANDSHAKE` — handles re-handshake (e.g., after peer restart)
   - Ignores `CHAT` and `FILE` envelopes (those come through the inbound accept_loop)

### Inbound Handler Thread

`handle_inbound_connection` processes all envelopes on accepted connections:
1. Decrypts if ciphertext is present
2. Dispatches based on inner message type:
   - `CHAT` → `handle_chat_message` → sends `DELIVERY_ACK` back
   - `HANDSHAKE` → verifies signature, derives session key, sends response
   - `FILE_OFFER`, `FILE_CHUNK`, `FILE_COMPLETE` → file transfer handling
   - `INFO_REQUEST`, `INFO_RESPONSE` → identity info exchange

## Envelope Protocol

All TCP messages are length-prefixed protobuf `Envelope` messages:

```
[4 bytes: length][N bytes: serialized Envelope]
```

### Envelope Types

| Type | Direction | Encrypted | Purpose |
|------|-----------|-----------|---------|
| `HANDSHAKE` | Bidirectional | No | Key exchange and identity verification |
| `CHAT` | Both | Yes | Chat messages (also used as wrapper type for encrypted payloads) |
| `DELIVERY_ACK` | Bidirectional | Yes | Acknowledge message receipt |
| `FILE_OFFER` | Sender → Receiver | Yes | Propose a file transfer |
| `FILE_RESPONSE` | Receiver → Sender | Yes | Accept or reject a file offer |
| `FILE_CHUNK` | Sender → Receiver | Yes | File data chunk |
| `FILE_COMPLETE` | Sender → Receiver | Yes | Signal transfer completion |
| `INFO_REQUEST` | Either | Yes | Request peer's personal info |
| `INFO_RESPONSE` | Either | Yes | Return personal info entries |

## Reconnection Handling

### Peer Restart

When a peer restarts:
1. Their session key is lost (ephemeral, in-memory)
2. They send a new HELLO with the same Ed25519 key
3. The existing session is cleared (`clearSession`)
4. A fresh handshake is initiated on the next message send
5. Messages are queued until the handshake completes

### Session Churn Prevention

`connect_to` only initiates a handshake if `!session_manager_->isReady(peer_name)`. If a session already exists, the existing outbound fd is reused. This prevents creating a new session for every message.

## Debug Categories

| Category | Component | Output |
|----------|-----------|--------|
| `UDPB` | UdpHelloBroadcaster | UDP packet send/receive |
| `CRYPTO` | KeyManager, SessionManager | Key generation, handshakes, encryption/decryption |
| `MESSAGE` | ChatServer | Message send/receive, ACKs |
| `RENDER` | TerminalUI | UI draw calls, layout |
| `NET` | ChatServer | TCP connect, accept, disconnect |
| `FILE` | ChatServer | File transfer progress |
