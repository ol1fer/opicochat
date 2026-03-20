# changelog

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
