#pragma once
#include <string>
#include <cstdint>
#include <vector>

std::string trim(const std::string& s);
bool iequals(const std::string& a, const std::string& b);
std::string sanitize_message(const std::string& s);
bool parse_host_port(const std::string& input, std::string& host, uint16_t& port, uint16_t def_port);

// ANSI helpers
std::string ansi_for_hex(const std::string& hex); // "\x1b[38;2;R;G;Bm"
std::string ansi_reset();
std::string ansi_dim();
std::string ansi_italic();
std::string ansi_bold();

#ifdef _WIN32
void enable_windows_ansi();
#endif

// Username policy: alphanumeric + underscore, 2-20 chars, can't start with underscore
bool is_valid_username(const std::string& name);
bool is_reserved_name(const std::string& name); // [server], server, etc.

// Color helpers
bool is_valid_hex_color(const std::string& in);
std::string normalize_hex_hash(const std::string& in); // "#RRGGBB" or ""

// Terminal color
bool term_supports_truecolor();

// Colorize a name for display:
//   [server]  → pink bracketed
//   username  → preferred hex or deterministic from name hash
std::string colorize_name(const std::string& name, const std::string& preferred_hex);

// Build the colored timestamp prefix "[HH:MM] "
std::string format_ts_prefix(const std::string& iso);

// Terminal attach (Linux/macOS: open a terminal if launched from GUI)
bool ensure_terminal_attached(int argc, char** argv);

// Self-update helpers
// Returns the absolute path of the running executable
std::string get_self_exe_path();
// Fetches URL as text (uses curl on Linux/mac, powershell on Windows)
std::string update_http_get(const std::string& url);
// Extracts "tag_name" string from GitHub releases JSON (e.g. "v1.5")
std::string update_get_latest_tag(const std::string& json);
// Finds browser_download_url for a named asset in GitHub releases JSON
std::string update_find_asset_url(const std::string& json, const std::string& asset_name);
// Downloads URL to dest_path. Returns true on success.
bool update_download_file(const std::string& url, const std::string& dest_path);
// Replaces exe_path with downloaded_path atomically (Linux/macOS only). Sets msg.
bool update_apply_binary(const std::string& downloaded_path,
                         const std::string& exe_path, std::string& msg);
// Writes opicochat[client|server]updater.bat beside exe_path. Returns bat path or "".
// The bat downloads the latest release from GitHub and replaces the binary.
// Windows only; returns "" on other platforms.
std::string update_write_bat(const std::string& exe_path, bool is_server);
// Launches a file in the background (Windows: start ""). No-op on other platforms.
void update_launch_file(const std::string& path);
