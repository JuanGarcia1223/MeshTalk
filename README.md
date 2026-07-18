```
           __  ___          __  ______      ____  
          /  |/  /__  _____/ /_/_  __/___ _/ / /__
         / /|_/ / _ \/ ___/ __ \/ / / __ `/ / //_/
        / /  / /  __(__  ) / / / / / /_/ / / ,<   
       /_/  /_/\___/____/_/ /_/_/  \__,_/_/_/|_|  
```

**A peer-to-peer, end-to-end encrypted chat app for your local network: no server, no accounts, no internet required.**

## What it is

Meshtalk turns any LAN into a private group chat. Run it on a couple of machines on the same network and peers find each other automatically over UDP broadcast: no server to stand up, no config file to write, no account to create. Every conversation is end-to-end encrypted (X25519 key exchange + Ed25519 signatures), so even someone sniffing traffic on the same network can't read messages exchanged between trusted peers. It's a single terminal binary with a fast notcurses UI, and it can send files as well as text.

## Why it exists

Most chat apps assume a server you don't control: an account, a cloud provider, and metadata retained indefinitely. On a local network (home LAN, office, lab, conference room, CTF), that's dependency you don't need. Meshtalk fixes that directly:

- **No server, no account**: peers discover each other on the LAN and talk directly, peer to peer.
- **Trust without an identity provider**: every peer has an Ed25519 keypair; you verify a peer's key fingerprint once and they stay trusted until that key changes.
- **Works with no internet**: as long as machines share a LAN, Meshtalk works, air-gapped or not.

## Who it's for

- Developers and self-hosters who want private LAN chat without standing up infrastructure
- Security/CTF folks who need quick, encrypted comms on an isolated network
- Terminal users who'd rather not leave the terminal to chat
- Anyone learning P2P discovery, E2E crypto, or TUI development: the codebase is small and easy to read end to end

## Core Features

- **End-to-end encryption**: X25519 ECDH session keys per peer pair, Ed25519 signatures for identity and authentication
- **Automatic discovery**: UDP broadcast finds peers on the same LAN with zero configuration
- **Persistent trust**: verify a peer's fingerprint once; they stay trusted across restarts until their key changes
- **File transfer**: send files directly to peers with progress tracking
- **Offline-first messaging**: messages queue and deliver once a peer is back online, with delivery acknowledgments
- **Local SQLite history**: messages, trust store, and identity persist locally, per user, with no cloud sync
- **Zero setup**: first run prompts once for a username and remembers it; no account, no server address to configure

## Platform Support

**Linux only.** Meshtalk depends on POSIX APIs (raw sockets, `ifaddrs`, terminal I/O via notcurses) throughout and has no Windows port planned. macOS is untested but may work since notcurses supports it; Linux is the only supported target.

**WSL works, but not with Windows Terminal.** If you're on WSL, do not run Meshtalk in Windows Terminal: its input handling doesn't report mouse events or shifted/symbol keys correctly, which breaks clicking and typing in the UI. Install a real Linux terminal emulator instead and run Meshtalk from there, such as [kitty](https://sw.kovidgoyal.net/kitty/), [Terminator](https://gnome-terminator.org/), [Alacritty](https://alacritty.org/), or [WezTerm](https://wezterm.org/).

## Prerequisites

- A Linux system (or WSL2, see the terminal warning above)
- CMake ≥ 3.24
- A C++17 compiler (g++ or clang++)
- git (used by CMake's `FetchContent` to pull dependencies at configure time)
- pkg-config
- Protobuf compiler + library
- notcurses' own build dependencies: terminfo/ncurses headers, libunistring, libdeflate, and (by default) FFmpeg's dev libraries, since notcurses is built from source with its default FFmpeg-backed multimedia engine

`libsodium`, `notcurses`, and `SQLiteCpp` (with SQLite itself) are pulled and built from source automatically by CMake at configure/build time, so you don't need to install them separately.

On Debian/Ubuntu:

```bash
sudo apt install build-essential cmake git pkg-config \
  protobuf-compiler libprotobuf-dev \
  libncurses-dev libunistring-dev libdeflate-dev \
  libavcodec-dev libavformat-dev libavdevice-dev libavutil-dev libswscale-dev
```

> Tip: Meshtalk never uses notcurses' image/video features, so you can skip the FFmpeg dependency entirely by configuring with `-DUSE_MULTIMEDIA=none` (see below).

## Building

```bash
git clone <repo-url> meshtalk
cd meshtalk
mkdir build && cd build
cmake ..                # add -DUSE_MULTIMEDIA=none to skip the FFmpeg dependency
make -j$(nproc)
```

The first configure pulls and builds libsodium, notcurses, and SQLiteCpp from source, so it takes a few minutes. Subsequent builds are fast.

## Running

```bash
./meshtak
```

The first run prompts for a one-word username and remembers it in `meshtalk.db`; every later run just works.

## Documentation

For detailed documentation (architecture, crypto, networking, UI, CLI, file transfer, identity), see [`docs/README.md`](docs/README.md).
