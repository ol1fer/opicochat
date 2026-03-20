# opicochat

a terminal chat server + client written in c++17. no external dependencies — just the standard library and platform sockets.

works on linux and windows. macos should work but is untested.

---

## features

- **end-to-end encryption** — x25519 key exchange + chacha20 stream cipher on every connection
- **admin & mod system** — key-based roles with separate permissions. mods can kick/mute/etc, admins can do everything plus manage other staff
- **stealth mode** — admins can go invisible in `/list` and chat (other admins still see a grey `a` badge)
- **rate limiting** — per-ip connection rate and concurrency limits. hitting the rate limit blocks that ip for a configurable duration
- **keepalive** — server pings clients on a configurable interval and kicks unresponsive ones
- **lockdown mode** — `/lockdown on` blocks all new connections instantly
- **message history** — optional replay of recent messages on join (default: 0 / disabled for privacy)
- **private messages** — `/dm <user> <message>`
- **mute** — admins/mods can mute users with optional duration
- **server-side commands** — `/nick`, `/me`, `/motd`, `/topic`, `/kick`, `/ban`, `/mute` and more
- **client logging** — optionally write received chat to a local log file, toggle with `/log on|off` or in settings
- **pause/play** — `/pause` buffers incoming chat while you look something up, `/play` replays it
- **server list with live ping** — saved servers are pinged on the connect screen showing latency, user count, and whether a password is required
- **colored server names** — servers can set a custom name color shown in the client list
- **per-user colors** — optional `#rrggbb` hex color per username (deterministic fallback color if none set)
- **ignore list** — hide messages from specific users client-side
- **saved usernames & servers** — store multiple usernames with colors and servers with passwords/keys

---

## build

requires cmake 3.20+ and a c++17 compiler.

### linux

```bash
cmake --preset linux-release
cmake --build --preset linux-release -j
```

outputs: `build-linux/opicochat_server` and `build-linux/opicochat`

### windows (msvc)

```bat
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release -j
```

### macos (untested)

```bash
xcode-select --install
brew install cmake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

---

## quick start

```bash
# terminal 1 — start the server
./opicochat_server

# terminal 2 — start the client
./opicochat
```

to make yourself admin while testing:

```
# in the server console:
/admin add yourname
# the key is dm'd to you in the client. save it with /key add or in settings → manage servers.
```

---

## server config

`opicochatserver.cfg` is created on first run next to the binary. missing keys are appended automatically when the server starts, so you can safely add new options to an existing config.

```ini
port=24816
server_name=opico chat
server_name_color=            # optional #rrggbb shown in client server list
motd=                         # message of the day (sent to clients on join)
password_enabled=0
password=

# connection limits
max_concurrent_per_ip=5       # max simultaneous connections from one ip
max_conn_per_min_per_ip=20    # accepted connections per minute from one ip
max_total_conn=200            # server-wide cap
ip_block_duration_secs=300    # how long to block an ip after hitting the rate limit

# message history replayed to new joiners (0 = disabled)
history_size=0

# message rate limiting (per user)
max_msg_per_window=6
msg_window_secs=4

# keepalive
keepalive_interval_secs=30    # how often to ping clients
keepalive_timeout_secs=10     # how long before kicking an unresponsive client

# logging
log_dir=logs
```

**other server files** (auto-managed, gitignored):

| file | purpose |
|---|---|
| `admins.cfg` | admin key → username mapping |
| `mods.cfg` | mod key → username mapping |
| `banned.cfg` | `ip:<addr>=<reason>` entries |
| `logs/YYYY-MM-DD.log` | daily server chat logs |

---

## client config

`opicochat.cfg` is created/updated by the client ui. you can also edit it directly.

key fields:

- `usernames` — semicolon-separated `name#RRGGBB` entries (color optional). e.g. `oliver#ff5cad;alice#00aaff;bob`
- `servers` — semicolon-separated `host:port` entries
- `server_admin_keys` — `host:port#key` entries (key sent automatically on connect)
- `server_passwords` — `host:port#password` entries (password sent automatically)
- `server_display_names` — custom label shown in the server list
- `show_timestamps` — `1` or `0`
- `show_server_ip` — `1` or `0` (show host:port in server list)
- `client_log_enabled` — `1` or `0` (write received chat to `logs/client-YYYY-MM-DD-<host>.log`)

---

## commands

### everyone

| command | description |
|---|---|
| `/help` | show available commands (staff see extra commands in green) |
| `/list` | list online users with badges and latency |
| `/list ping` | same but shows last known ping time |
| `/dm <user> <msg>` | send a private message |
| `/me <text>` | send an action message (`* name text`) |
| `/nick <name>` | change your username |
| `/color <#rrggbb>` | change your color |
| `/ping` | check your latency to the server |
| `/ignore <user>` | hide messages from a user (client-side) |
| `/unignore <user>` | stop ignoring a user |
| `/ignored` | show your ignore list |
| `/ts` | toggle timestamps |
| `/clear` | clear the screen (also ctrl+l) |
| `/pause` | buffer incoming chat so you can focus |
| `/play` or `/unpause` | replay buffered messages and resume |
| `/log on\|off` | toggle client-side logging to a local file |
| `/savenick [force\|new]` | save current username+color to your profile |
| `/saveserver [force\|new]` | save current server to your server list |
| `/key add` | save your current admin/mod key to this server entry |
| `/key show` | display your current session key |
| `/key elevate <key>` | elevate your session with a key |
| `/rc` or `/reconnect` | reconnect to the same server |
| `/dc` or `/disconnect` | disconnect |

### mod & admin (shown green in /help)

| command | description |
|---|---|
| `/kick <user> [reason]` | disconnect a user |
| `/mute <user> [duration]` | mute a user. duration: `30s`, `5m`, `1h` (default 1m) |
| `/unmute <user>` | remove a mute |
| `/mutelist` | list currently muted users |
| `/lookup <user>` | show ip and role info |
| `/motd <text>` | update the message of the day |
| `/topic <text>` | set the channel topic |
| `/health` | show server stats (uptime, connections, bans, keepalive, etc.) |
| `/lockdown on\|off` | block / allow new connections |

### admin only (shown green in /help)

| command | description |
|---|---|
| `/ban <user> [reason]` | ban a user by ip |
| `/banip <ip> [reason]` | ban a specific ip |
| `/unban <ip>` | remove a ban |
| `/banlist` | list banned ips |
| `/admin add <user>` | generate an admin key for a user |
| `/admin remove <user>` | revoke a user's admin |
| `/admin list` | list all admins |
| `/mod add <user>` | generate a mod key for a user |
| `/mod remove <user>` | revoke a user's mod |
| `/mod list` | list all mods |
| `/stealth` | toggle stealth mode (invisible in list and chat; other admins see a grey `a`) |
| `/reload bans\|admins\|mods` | reload config files without restarting |
| `/serverinfo` | show server config details |

### server console

all slash commands above work from the server's stdin. a plain line (no slash) broadcasts a message as `[server]`.

---

## admin & mod keys

**adding staff:**

```
# from the server console:
/admin add username     # generates an admin key, dm'd to that user
/mod add username       # generates a mod key, dm'd to that user
```

**as a client receiving a key:**

1. copy the key from the dm
2. type `/key add` to save it to the current server entry, or go to **settings → manage servers** and add it there
3. next time you connect from the saved server entry, the key is sent automatically

**one-off / manual:**

- go to **enter address manually** on the connect screen — you'll be prompted for an optional key
- this deliberately does *not* auto-apply saved keys, so you can connect without staff permissions if needed

---

## roles & badges

badges are shown next to names in chat and `/list`:

| badge | color | meaning |
|---|---|---|
| `a` | green | admin |
| `a` | grey | admin in stealth (visible to other admins only) |
| `m` | blue | mod |
| *(none)* | — | regular user |

mods cannot go stealth. stealth admins appear as normal users to everyone except other admins.

---

## encryption

every connection uses a fresh x25519 ephemeral key exchange. the resulting shared secret keys a chacha20 stream cipher for all traffic after the handshake. there are no persistent keys and no certificates needed — the encryption is opportunistic but protects against passive interception.

---

## networking

- default port: **24816** (ipv4/ipv6 dual-stack)
- per-ip rate limits enforced on accept: concurrent connections and a per-minute sliding window
- ips that exceed the rate limit are blocked for `ip_block_duration_secs` (default 5 minutes)
- a keepalive system pings each client on a staggered schedule and kicks unresponsive ones

---

## troubleshooting

- **windows shows escape codes** — use windows terminal or a recent powershell/cmd session
- **can't become admin** — make sure the key is saved for the exact `host:port` or enter it via **enter address manually**. check the server still has the key in `admins.cfg`
- **history shows 0 messages** — this is intentional by default for privacy. set `history_size=N` in the server config to enable it
- **client log location** — logs go to `logs/client-YYYY-MM-DD-<host>.log` next to the client binary
