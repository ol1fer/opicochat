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
