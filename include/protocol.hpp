#pragma once
#include <string>

namespace proto {

// welcome
std::string make_welcome(const std::string& server_name, bool pass_required);
bool parse_welcome(const std::string& line, std::string& server_name, bool& pass_required);

// auth (optionally includes color)
std::string make_auth(const std::string& user, const std::string& pass, const std::string& color_hex_opt);
bool parse_auth(const std::string& line, std::string& user, std::string& pass, std::string& color_hex_opt, bool& has_color);

// client message
std::string make_msg(const std::string& text);
bool parse_msg(const std::string& line, std::string& text);

// CHAT carries color as separate token (use "-" if none)
// Format: "CHAT <iso> <name> <color> <text...>"
std::string make_chat(const std::string& iso, const std::string& name, const std::string& color_hex_or_dash, const std::string& text);
bool parse_chat(const std::string& line, std::string& iso, std::string& name, std::string& color, std::string& text);

// legacy / unused
std::string make_req_history(int n);
bool parse_history_header(const std::string& line, int& n);

} // namespace proto
