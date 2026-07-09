# Identity System

Each Meshtalk user has a persistent cryptographic identity and an optional set of personal info key-value pairs.

## Cryptographic Identity

### Ed25519 Keypair

Your identity is an Ed25519 signing keypair:
- **Public key**: 32 bytes, broadcast to all peers via UDP discovery
- **Private key**: 64 bytes, stored encrypted in the SQLite database
- **Fingerprint**: First 16 bytes of public key formatted as `XXXX:XXXX:XXXX:XXXX`

The keypair is generated once on first run and persists across sessions. If you delete your database, a new identity is created.

### Fingerprint Format

```
B330:B60C:1F4D:7932
```

The fingerprint is the human-readable representation of your public key. Peers verify this out-of-band to confirm they are talking to you and not an impostor.

## Trust Establishment

### First Contact

When you see a peer for the first time:
1. Their username appears in the **Pending** section with a yellow dot
2. Clicking them opens the **Trust Identity** modal
3. The modal shows their fingerprint
4. You must choose **Accept** or **Reject**

### Accepting a Peer

- The peer moves to the **Trusted** section
- Their public key is stored in the database with `trust_status = "trusted"`
- You can now send and receive messages
- They remain trusted even when offline or across restarts

### Rejecting a Peer

- The peer stays in the **Pending** section
- Messaging is blocked
- Their key is stored with `trust_status = "pending"`
- You can trust them later by clicking them again

### Key Mismatch

If a trusted peer connects with a **different** public key:
1. A key mismatch modal appears showing both fingerprints
2. The peer is moved to the **mismatch** state
3. Messaging is blocked
4. You must explicitly re-trust them to resume communication

This protects against man-in-the-middle attacks where someone impersonates a known peer.

## Personal Info

You can configure key-value pairs that describe yourself. These are sent to peers when they request your info via `/INFO`.

### Managing Info

Use the personal info editor:
```bash
./meshtalk_demo --name alice --edit-info
```

Options:
1. **Dump** — List all entries
2. **Edit** — Modify an existing entry
3. **Add** — Create a new entry
4. **Delete** — Remove an entry

### Example Entries

| Key | Value |
|-----|-------|
| `name` | Alice Smith |
| `email` | alice@example.com |
| `pgp` | 0xABCD1234 |
| `twitter` | @alice |

### Fetching Peer Info

Select a peer and type `/INFO`. Their info entries appear in an identity popup alongside their fingerprint.

## Identity Popup

The identity popup displays:
- Peer name
- Ed25519 fingerprint
- Personal info entries (if available)
- Trust status

Click **Close** or press **Esc** to dismiss.

## Security Considerations

- **No central authority**: There is no CA or central server. Trust is established peer-to-peer via fingerprint verification.
- **Out-of-band verification**: You should verify fingerprints in person or through another trusted channel.
- **Database protection**: The SQLite database contains your private key. Protect it like an SSH key.
- **Key rotation**: There is no automated key rotation. If your key is compromised, delete the database and re-trust all peers.

## Database Schema

### identity table

| Column | Type | Description |
|--------|------|-------------|
| id | INTEGER PRIMARY KEY | Row ID |
| public_key | BLOB | Ed25519 public key (32 bytes) |
| private_key | BLOB | Ed25519 private key (64 bytes) |

### known_peers table

| Column | Type | Description |
|--------|------|-------------|
| id | INTEGER PRIMARY KEY | Row ID |
| username | TEXT UNIQUE | Peer username |
| public_key | BLOB | Peer Ed25519 public key |
| trust_status | TEXT | `"trusted"` or `"pending"` |
| first_seen | INTEGER | Unix timestamp |

### info_entries table

| Column | Type | Description |
|--------|------|-------------|
| id | INTEGER PRIMARY KEY | Row ID |
| key | TEXT UNIQUE | Entry key |
| value | TEXT | Entry value |
| updated_at | INTEGER | Unix timestamp |

### peer_info table

| Column | Type | Description |
|--------|------|-------------|
| id | INTEGER PRIMARY KEY | Row ID |
| peer_name | TEXT | Peer username |
| key | TEXT | Entry key |
| value | TEXT | Entry value |
