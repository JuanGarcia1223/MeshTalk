# File Transfer

Meshtalk supports direct peer-to-peer file transfer over the existing encrypted TCP connections.

## Overview

Files are transferred in chunks over the same encrypted channel used for chat messages. The protocol uses SHA-256 for integrity verification.

## Transfer Flow

### Sending a File

1. User selects `/UPLOAD` and picks a file
2. `ChatServer::send_file_offer` reads the file and computes SHA-256
3. A `FILE_OFFER` envelope is sent to the peer containing:
   - `transfer_id` (UUID)
   - `filename`
   - `file_size`
   - `sha256_hash`
   - `from_user`
4. Peer receives the offer and shows it in chat as an attachment message
5. If accepted, sender begins sending `FILE_CHUNK` envelopes
6. After all chunks are sent, a `FILE_COMPLETE` envelope signals completion

### Receiving a File

1. `handle_file_offer` creates an `IncomingFileTransfer` record
2. `handle_file_chunk` appends each chunk to the transfer buffer
3. `handle_file_complete` verifies the SHA-256 hash
4. The file is stored in the database via `saveFileData`
5. A download popup can later retrieve it with `download_file`

### Chunking

Files are split into manageable chunks for transmission. Each chunk is encrypted with the session key and sent as a `FILE_CHUNK` envelope. The receiver reassembles chunks in order.

## UI Flow

### Upload

1. Type `/UPLOAD` or select it from the command menu
2. A file picker dialog opens
3. Select a file and confirm
4. Upload popup appears showing:
   - Filename
   - Target peer
   - Progress bar
   - Bytes sent / total size
5. Wait for transfer to complete or click Cancel

### Download

1. Type `/DOWNLOAD` or select it from the command menu
2. Download popup shows all completed transfers
3. Select a transfer and confirm
4. Choose a destination directory
5. File is written to disk

## File Storage

### Database Storage

Received files are stored as BLOBs in the SQLite database:
- Table: `file_transfers`
- Column: `file_data`

This ensures files survive application restarts.

### Storage Path

The database and file storage are located at:
```
~/.meshtalk/<username>.db
```

## Security

- File transfers use the same encrypted channel as chat messages (XSalsa20-Poly1305)
- SHA-256 hash verification ensures file integrity
- File offers are only accepted from trusted peers
- File data is encrypted in transit and at rest (in the database)

## Limitations

- Large files may cause UI lag during transfer
- No resume capability for interrupted transfers
- No bandwidth throttling
- Files are stored in SQLite as BLOBs, which may not be efficient for very large files
