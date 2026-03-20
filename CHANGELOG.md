# changelog

## v2.4
- **windows updater** — relaunch prompt now defaults to yes (`[Y/n]`); pressing enter relaunches automatically

## v2.3
- **check for updates menu** — now shows a proper yes/no sub-menu when an update is available instead of directing to `/updateclient confirm`
- **force reinstall in menu** — when already up to date, an options menu offers force reinstall
- **linux relaunch loop fix** — updating via `/updateclient confirm` in a chat session no longer causes the relaunched client to spam the menu. the chat session holds the terminal in raw mode; the terminal is now reset to normal before `execl` so the new process starts cleanly

## v2.2
- **banned usernames** — new `banned_names=admin,mod` config field; comma-separated list of usernames blocked at login. `server` is always blocked separately and cannot be removed
- **`/skick` and `/sban`** — silent kick/ban; shows as `disconnected` with no public announcement. `/sban` also adds the IP to the ban list
- **kick/ban announcements** — `/kick` and `/ban` now broadcast a notice to all users before the connection drops
- **disconnect messages** — all leave messages now say `disconnected`. kicked/banned users are announced separately via notices
- **`/dm server`** — users can `/dm server <text>` to send a message to the server console
- **server console `/dm`** — server operator can `/dm <user> <text>` to send a direct message to any connected user
- **`/motd` persistence** — setting the motd via `/motd <text>` now saves it to `opicochatserver.cfg` so it persists across restarts
- **motd on startup** — server prints the loaded motd on startup so you can confirm it was read from config
- **`/reload config`** — also rebuilds the banned names set
- **`/nick` validation** — renaming to a banned username is now blocked

## v2.1
- **freeze fix** — added a 5s send timeout on all client sockets to prevent a slow or disconnecting client from blocking the server's select loop and freezing all other users
- **ping on join** — server sends an initial ping immediately after a client authenticates, so `/list ping` always shows a value rather than `?`
- **`/list ping` header** — now shows `online (N) — ping:` before listing users
- **`/reload config`** — new option for `/reload` that reloads `opicochatserver.cfg` at runtime, including updating the motd
- **motd format** — motd now displays as `~ text ~` instead of `~ motd: text ~`
- **dm colors** — sender name in direct messages is now colorized using their chat color
- **`/key add` fix** — fixed remaining references that still said `/adminkey add`
- **settings prompt** — "admin key" prompt in server settings renamed to "admin/mod key"
- **check for updates menu** — reworked update flow (further improved in v2.3)
- **batch file auto-close** — windows update batch file now closes its window automatically after completing

## v2.0
- **windows version string fix** — fixed version showing as `$version` on windows builds; version is now written to a generated header at cmake configure time, bypassing compiler flag quoting issues

## v1.6
- **/updateclient force** and **/updateserver force** — skip version check and force a reinstall of the latest binary
- **automatic windows updater** — `confirm`/`force` on windows writes a named batch file (`opicochatclientupdater.bat` / `opicochatserverupdater.bat`) and launches it automatically. the server closes itself after launching; the client disconnects and exits. the batch file waits in a loop until the exe unlocks, replaces it, then relaunches. server batch asks yes/no before relaunching; client auto-relaunches.
- **re-usable batch files** — the batch files always pull from `releases/latest/download/` so they can be re-run any time without using the in-app command
- **auto-relaunch on linux/macos** — after a successful update, the client replaces itself via `execl()`. the server applies atomically and prompts you to use `/restart`
- **/restart** — new server console command (linux/macos) to relaunch the server process in-place via `execl()` after an update
- **fixed github json parsing** — version and asset url extraction now handles any whitespace formatting in the api response, fixing the "could not parse response from github" error

## v1.5
- **/updateserver** — server console command to check for and apply server binary updates from github
- **/updateclient** — client-side command to check for and apply client binary updates from github
- **check for updates** — option 3 in the client main menu. checks github, offers to apply immediately
- update flow: run the command first to check, then run with `confirm` to apply. running confirm without checking first gives a clear message
- linux/macos: update replaces the binary atomically (no restart of running process needed until you choose to)
- windows: downloads the new binary and writes a self-deleting `update.bat` to apply after closing
- duplicate usernames are already blocked at login — `/inspect` always finds at most one match

## v1.4
- **/inspect** — renamed from `/info` (`/lookup` in docs). now shows ip, role, version, color, uptime, ping, mute status, and stealth status (stealth only shown to admins)

## v1.3
- updated readme: version enforcement docs, macos arm only note, client logging, icons
- windows executables have icons embedded (opicochat.ico / opicochatserver.ico)

## v1.2
- **version enforcement** — server rejects clients on a mismatched version by default. configurable via `allow_version_mismatch` in server config. rejection message shows both server and client versions
- **git-tag versioning** — app version is now derived from the git tag at build time. binaries automatically reflect the release version (e.g. tagging `v1.2` produces binaries that say `v1.2`). falls back to `"dev"` for local builds without a tag
- **macos arm64 build** — added apple silicon build to the release workflow
- dropped macos x64 build (github retired the macos-13 runner)
- release workflow now passes the tag version to cmake for all platforms

## v1.1
- **encryption** — x25519 ephemeral key exchange + chacha20 stream cipher on all connections
- **admin & mod system** — separate key-based roles. admins can manage staff, mods can kick/mute
- **stealth mode** — admins only. invisible in `/list` and chat. other admins see a grey `a` badge. mods cannot stealth
- **keepalive** — server pings each client on a staggered schedule and kicks unresponsive ones. configurable via `keepalive_interval_secs` and `keepalive_timeout_secs`
- **rate limit ip blocking** — ips that hit the connection rate limit are blocked for a configurable duration (`ip_block_duration_secs`, default 5 minutes)
- **lockdown mode** — `/lockdown on|off` blocks all new connections instantly (mod+admin)
- **pause/play** — `/pause` buffers incoming messages (up to 500), `/play` or `/unpause` replays them
- **client logging** — `/log on|off` writes received chat to `logs/client-YYYY-MM-DD-<host>.log`. also toggleable in settings
- **history defaults to 0** — message history replay on join is disabled by default for privacy. set `history_size=N` in server config to enable
- **version shown** — client main menu and server startup both display the app version
- **server name colors** — servers can set `server_name_color=#rrggbb` to colorize their name in the client list
- **colored /help** — staff and admin commands shown in green when running `/help`
- **stealth visibility fix** — stealth admins (sadmin role) are hidden from mods, only visible to other admins
- **manual connect key isolation** — entering an address manually no longer auto-applies saved admin/mod keys
- **/key command** — renamed from `/adminkey`. saves/shows/elevates admin or mod keys
- **saved nickname colors** — `/savenick` now correctly saves and displays the current color
- **show_server_ip toggle** — settings option to show/hide host:port in the server list (default off)
- **github actions release workflow** — automatic builds for linux x64, windows x64, macos arm64 on tag push
- **gitignore** — added `CLAUDE.md`, `admins.cfg`, `mods.cfg`, `banned.cfg`

## v1.0
- initial release
- linux and windows builds
- basic chat server and client
- ansi colors, per-user hex colors
- admin keys and ban system
- per-ip rate limiting
- server console with slash commands
- saved usernames and servers
- message history
- private messages, /me actions, /nick
