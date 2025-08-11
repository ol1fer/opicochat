# opicochat

A tiny, terminal-first chat server + client written in C++17.
Works on Linux and Windows; macOS should work but is currently untested.

* Dual binaries: `opicochat_server`, `opicochat_client`
* ANSI color names with optional per-user hex (e.g. `#FF5CAD`)
* Admin keys, bans, and simple server console controls
* Per-IP rate limits (concurrent + per-minute)
* One-off “Direct connect” and “One-off username” (no saving)

The client and server talk over a small, line-based protocol (WELCOME/AUTH/MSG/CHAT). The welcome line carries the human-readable server name and whether a password is required; names with spaces are supported.

---

## Build

### Requirements

* CMake ≥ 3.20
* A C++17 compiler:

  * **Linux:** GCC 10+ or Clang 12+
  * **Windows:** MSVC (Visual Studio 2019/2022 or the Build Tools)
  * **macOS (untested):** Apple Clang (Xcode Command Line Tools)
* (Optional) Ninja build system

### Linux

```bash
cmake --preset linux-release
cmake --build --preset linux-release -j
```

### Windows (Visual Studio / MSVC)

```bat
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release -j
```


> Tip: The client enables Windows VT/ANSI so colors render in modern terminals. If your terminal still shows escape codes, try Windows Terminal or a recent PowerShell/CMD session. (The client calls `enable_windows_ansi()` at startup.)

### macOS (untested)

```bash
# install tools
xcode-select --install
brew install cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

---

## Run

### Start the server

```bash
./opicochat_server
```

First run creates `opicochatserver.cfg` next to the binary with sensible defaults. Logs go under `logs/` (one file per day).

### Start the client

```bash
./opicochat
```

* **Manage usernames/servers** from **Settings**.
* Or **Connect to server** and use:

  * **Direct connect (one-off)** to enter a host\:port just for this session (it won’t be saved).
  * **One-off username** to use a name (and optional color) just for this session (it won’t be saved).

---

## Configuration

### Server config: `opicochatserver.cfg`

Generated on first run. New key names (the server also accepts an older naming set for backward compatibility). Defaults shown below.

```ini
port=24816
server_name=an opico chat server
password_enabled=0
password=

# Limits
max_concurrent_per_ip=8          ; maximum simultaneous authed connections from one IP
max_conn_per_min_per_ip=20       ; accepted connections per minute from one IP
max_total_conn=200               ; server-wide cap

# Files
log_dir=logs
motd=
```

Notes:

* `server_name` **can include spaces**; it shows up in the *“Connected to … as …”* banner the client prints. Other server messages display as `[server]`.
* Per-IP limits are enforced during accept: concurrent authed sessions and a per-minute sliding window. (Keys map to `cfg.max_conn_per_ip` and `cfg.max_conn_per_sec_per_ip` internally.)

Other server files (auto-managed):

* `logs/` — daily chat logs (the server picks a unique file per day).
* `banned.cfg` — simple “ip → reason” store.
* `admins.cfg` — admin key DB (key → original/last username).

### Client config: `opicochat.cfg`

Created/updated by the client UI. Key fields:

* `usernames` — semicolon list of entries `name#HEX`. `HEX` is optional (e.g. `oliver#FF5CAD;alice#00AAFF;bob`).
* `servers` — semicolon list `host:port` (e.g. `127.0.0.1:24816;chat.example.com:24816`).
* `server_admin_keys` — semicolon list of `host:port#KEY` (used to authenticate as admin when connecting to that server).

---

## Commands

### Everyone (in chat)

Type these in the client chat input:

* `/help` — show available commands (admins see admin commands too).
* `/list` — list online users.
* `/dc` or `/disconnect` — disconnect the current session.
* `/rc` or `/reconnect` — reconnect to the same server with the same username.

### Admin (in chat)

You must connect with a valid admin key (see “Admin keys” below).

* `/lookup <user>` — show IP and whether they’re admin.
* `/kick <user> [reason]` — kick a user.
* `/ban <user> [reason]` — ban by the user’s IP (and disconnect them).
* `/banip <ip> [reason]` — ban a specific IP.
* `/unban <ip>` — remove a ban.
* `/banlist` — list banned IPs.
* `/reload bans|admins` — reload `banned.cfg` or `admins.cfg`.
* `/admin add <user>` — generate an admin key for `<user>` (DM’d to them and logged to console).
  `/admin remove <user>` — revoke that user’s admin (by key).
  `/admin list` — list current admins (original/last usernames).

### Server console (stdin of `opicochat_server`)

You can type the same slash commands as above directly into the server console.
Typing a non-slash line sends a chat line from `[server]`.

---

## Admin keys

1. On the server console: `/admin add <username>` → a random 32-char key is generated and DM’d to that user and stored in `admins.cfg`.
2. On the client:

   * Save it per server in **Settings → Manage servers** (the key will be used automatically), **or**
   * Use **Direct connect (one-off)** and paste the key for just that session. (The client passes the key as the optional 4th AUTH token.)

You’ll see `admin privileges granted` shortly after connecting if the key is valid.

---

## Colors & usernames

* Usernames must be alphanumeric or `_` (underscores).
* Optional color: `#RRGGBB` or `RRGGBB`. The client normalizes the hex (or ignores it if invalid).
* If you don’t set a hex, a pleasant, deterministic color is derived from your name. (Client-side hashing → HSL → RGB → ANSI.)
* UI shows a colored preview of saved usernames. For menus, the client strips the surrounding `<…>` so it renders cleanly in lists.

---

## Networking

* Default port: **24816** (IPv4/IPv6 dual-stack listener).
* Per-IP limits (configurable): concurrent authed sessions and accepts per minute (sliding window). On excess, the server rejects with a clear error.

---

## Troubleshooting

* **Windows terminal shows escape codes:** Use Windows Terminal or a recent PowerShell/CMD. The client requests VT processing, but very old consoles may still not render 24-bit colors.
* **“Invalid color hex” on connect:** Make sure your hex looks like `#A1B2C3` (or `A1B2C3`). The client sends a normalized hex or none at all.
* **Can’t become admin:** Verify the admin key is saved for that exact `host:port` or supplied via Direct connect, and that the server still has that key in `admins.cfg`.

---

## Quick start (local)

```bash
# 1) Start the server
./opicochat_server

# 2) In another terminal, start the client and connect to 127.0.0.1:24816
./opicochat
```

To make yourself admin while testing:

```text
# in the server console:
/admin add yourname
# copy the key, then either save it in the client’s Manage servers, or use Direct connect once.
```
