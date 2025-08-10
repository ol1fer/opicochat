#pragma once
#include <string>
#include <cstdint>
#include <vector>

std::string trim(const std::string& s);
bool iequals(const std::string& a, const std::string& b);
bool contains_brackets(const std::string& s);
std::string sanitize_message(const std::string& s);

// parse host[:port] with default
bool parse_host_port(const std::string& input, std::string& host, uint16_t& port, uint16_t def_port);

// ANSI color
std::string ansi_for_hex(const std::string& hex); // "\x1b[38;2;R;G;Bm"
std::string ansi_reset();

#ifdef _WIN32
void enable_windows_ansi();
#endif

// Username policy
bool is_valid_username(const std::string& name);

// HEX color helpers
bool is_valid_hex_color(const std::string& in);        // accepts "#RRGGBB" or "RRGGBB"
std::string normalize_hex_hash(const std::string& in); // returns "#RRGGBB" or "" if invalid

// Terminal & color helpers
bool term_supports_truecolor();
std::string colorize_name(const std::string& name, const std::string& preferred_hex); // keeps brackets white; [server] always pink

// If launched outside a terminal, open one and re-run self (Linux/macOS)
bool ensure_terminal_attached(int argc, char** argv);
