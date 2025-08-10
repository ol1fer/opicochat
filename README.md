# opico chat (CLI)

Cross‑platform C++17 CLI chat app with server & client, built with CMake. 
- TCP, line‑based protocol
- Hostname/IP + default port 24816
- Server config auto‑generated on first run as `opicochat.cfg`
- Client looks for `opicochat.cfg` in the working directory (not auto‑generated; create via Settings → Generate)
- Server logs to `logs/YYYY-MM-DD.log` and maintains last 50 messages in memory (sent to clients on connect when they request it)
- Rate limiting: max connections per IP and per‑second per IP
- Optional server password; server can chat via its console and appears as `[server]` by default
- Usernames containing `[` or `]` are rejected
- Client commands: `/dc`, `/disconnect`, `/rc`, `/reconnect`

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

Run server:
```bash
./build/opicochat_server
```

Run client:
```bash
./build/opicochat_client
```

On Windows (PowerShell):
```powershell
cmake -S . -B build -G "NMake Makefiles"
cmake --build build -- /m
build\\opicochat_server.exe
```

## Config

**Server (auto‑generated `opicochat.cfg`):**
```ini
port=24816
server_name=[server]
password_enabled=false
password=
max_conn_per_ip=5
max_conn_per_sec_per_ip=3
max_total_conn=200
log_dir=logs
```

**Client (`opicochat.cfg`, created via Settings → Generate):**
```ini
last_host=example.com
last_port=24816
last_username=alice
usernames=alice#FF3366;bob#33CC99
servers=example.com:24816;chat.local:24816
```

## Notes
- Truecolor ANSI is used for coloring your own messages; Windows console VT mode is enabled automatically.
- Log rotation happens on date change.
- History is an in‑memory ring buffer persisted only in daily logs.
