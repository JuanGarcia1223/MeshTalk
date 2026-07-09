# User Interface

Meshtalk uses [notcurses](https://notcurses.com/) for a rich terminal-based user interface. It supports mouse clicks, keyboard navigation, and scrolling.

## Layout

The screen is divided into three panels:

```
+-----------+-----------------------------------+------------------+
|           |                                   |                  |
|  People   |           Chat Window             |   Debug Console  |
|  List     |                                   |   (optional)     |
|           |                                   |                  |
|           |-----------------------------------|                  |
|           |  Input Box                        |                  |
+-----------+-----------------------------------+------------------+
```

### People List (Left Panel)

Shows all known peers in two sections:

- **Trusted** (top): Self always first, then trusted peers sorted alphabetically
  - Green dot (●) = online
  - Gray dot (○) = offline
  - Blue badge = unread messages
- **Pending** (bottom): Peers seen on the network but not yet trusted
  - Yellow dot = pending trust decision

Click a peer to select them. Use ↑/↓ arrow keys to navigate.

### Chat Window (Center Panel)

The main message area. Title shows:
- `Chat: self` — when talking to yourself
- `Chat: alice(online)` — trusted peer who is online
- `Chat: alice(offline)(untrusted)` — untrusted peer who is offline

Messages are displayed as:
- `S: hello` — message you sent
- `R: hi there` — message you received
- `📎 file.pdf (12.3 KB)` — file attachment

When `--enable-ack-show` is passed, pending messages show `S:` with a **red background** until the delivery ACK is received.

### Input Box (Bottom of Chat)

Type messages here. Press Enter to send. Type `/` to show the command menu.

### Debug Console (Right Panel)

Shown only when `--debug` is enabled. Displays internal log messages, crypto debug output, and network events.

## Navigation

| Key | Action |
|-----|--------|
| ↑ / ↓ | Select previous/next peer |
| Mouse click | Select peer, click UI elements |
| Enter | Send message or execute selected command |
| `/` | Open command menu |
| Esc | Close popup/modal |
| Tab | Switch between trust modal buttons |

## Command Menu

Type `/` in the input box to open the command menu:

| Command | Description |
|---------|-------------|
| `/HI` | Send a hello message |
| `/BYE` | Send a goodbye message |
| `/STATUS` | Show peer status in debug console |
| `/SELFKEY` | Show your own public key fingerprint |
| `/INFO` | Fetch peer's personal info |
| `/CLEAR` | Clear chat history for selected peer |
| `/UPLOAD` | Upload a file to the selected peer |
| `/DOWNLOAD` | Download received files |

## Trust Modal

When you click a pending peer for the first time, a trust modal appears:

```
+----------------------------+
|  Trust Identity            |
|                            |
|  Peer: alice               |
|                            |
|  Fingerprint:              |
|  B330:B60C:1F4D:7932...    |
|                            |
|  Verify this fingerprint   |
|  out-of-band!              |
|                            |
|  [ Accept ]  [ Reject ]    |
+----------------------------+
```

- **Accept**: Peer becomes trusted, can send/receive messages
- **Reject**: Peer stays pending, messaging is blocked

## Identity Popup

Click `/SELFKEY` or receive an `/INFO` response to see the identity popup showing the peer's fingerprint and personal info entries.

## File Transfer UI

### Upload Popup

Shows filename, target peer, file size, and progress bar. Click Cancel to abort.

### Download Popup

Lists all completed file transfers. Select one and choose a download directory.

## Alerts

Alert popups appear for:
- Trying to message an offline peer
- Trying to upload a file to an offline peer
- Attempting to trust a peer with a key mismatch

## Scrolling

Scroll up in the chat window with the mouse wheel or by dragging. Scroll down to the bottom to auto-scroll with new messages.
