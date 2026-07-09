# Command-Line Interface

## Startup Flags

```bash
./meshtalk_demo --name <username> [options]
```

### Required

| Flag | Description |
|------|-------------|
| `--name <name>` | Your one-word username (required) |

### Optional

| Flag | Description |
|------|-------------|
| `--debug [CAT1,CAT2,...]` | Enable debug console and optional category filtering |
| `--enable-ack-show` | Show red background on `S:` prefix for pending messages |
| `--noui` | Run without UI (background discovery only) |
| `--edit-info` | Launch the personal info editor TUI |

### Debug Categories

When using `--debug`, you can optionally specify comma-separated categories:

| Category | Component | Output |
|----------|-----------|--------|
| `UDPB` | UdpHelloBroadcaster | UDP packet send/receive |
| `CRYPTO` | KeyManager, SessionManager | Key generation, handshakes, encryption/decryption |
| `MESSAGE` | ChatServer | Message send/receive, delivery ACKs |
| `RENDER` | TerminalUI | UI draw calls and layout decisions |
| `NET` | ChatServer | TCP connect, accept, disconnect events |
| `FILE` | ChatServer | File transfer offers, chunks, completion |

**Examples:**

```bash
# Enable all debug categories
./meshtalk_demo --name alice --debug

# Enable only UDP and crypto debug
./meshtalk_demo --name alice --debug UDPB,CRYPTO

# Enable message and network debug
./meshtalk_demo --name alice --debug MESSAGE,NET

# Enable pending ACK visual indicator
./meshtalk_demo --name alice --enable-ack-show --debug MESSAGE
```

### `--enable-ack-show`

When enabled, messages you send show a red background on the `S:` prefix until the peer sends a delivery acknowledgment back. This is useful for verifying the ACK system is working but can be visually noisy.

Without this flag, pending messages look identical to delivered ones.

## Slash Commands

Type `/` in the input box to open the command menu.

### `/HI`

Sends a simple "hello" chat message to the selected peer. Useful for testing connectivity.

### `/BYE`

Sends a goodbye message and disconnects from the selected peer. The peer will be marked offline.

### `/STATUS`

Prints the selected peer's status to the debug console:
- Online/offline state
- Trust status (trusted/pending/mismatch)
- IP address and port

### `/SELFKEY`

Displays your own Ed25519 public key fingerprint in an identity popup. Share this with peers for out-of-band verification.

### `/INFO`

Requests the selected peer's personal info entries (key-value pairs they have configured). Results appear in an identity popup. If the trust modal is open for that peer, the info updates inline.

### `/CLEAR`

Shows a confirmation modal. If confirmed, clears all chat history for the selected peer from the UI and database.

### `/UPLOAD`

Opens a file picker dialog. Select a file to send to the currently selected peer. An upload popup shows progress.

**Requirements:**
- Peer must be online
- Peer must be trusted

### `/DOWNLOAD`

Opens a download manager showing all completed file transfers. Select a transfer and choose a destination directory.

## Personal Info Editor

Launch with `--edit-info` to manage your personal info key-value pairs without starting the full chat UI:

```bash
./meshtalk_demo --name alice --edit-info
```

Options:
1. **Dump existing entries** — List all key-value pairs
2. **Edit entry** — Change the value of an existing key
3. **Add entry** — Create a new key-value pair
4. **Delete entry** — Remove a key-value pair
9. **Exit**

These entries are sent to peers when they request your info via `/INFO`.

## Examples

```bash
# Basic usage
./meshtalk_demo --name alice

# With debug output for network troubleshooting
./meshtalk_demo --name alice --debug UDPB,NET,MESSAGE

# With ACK indicator and crypto logging
./meshtalk_demo --name alice --enable-ack-show --debug CRYPTO,MESSAGE

# Background mode (no UI, just discovery)
./meshtalk_demo --name alice --noui

# Edit personal info
./meshtalk_demo --name alice --edit-info
```
