#pragma once
#include <string>

// Application and protocol versions
static constexpr const char* APP_VERSION   = "1.1";
static constexpr const char* PROTO_VERSION = "3";

namespace proto {

// ---- Encryption handshake (unencrypted) ----
// HELLO <x25519_pub_hex_64chars>
std::string make_hello(const std::string& pub_hex);
bool parse_hello(const std::string& line, std::string& pub_hex);

// ---- Server-info ping (unencrypted, before HELLO response) ----
// PING  (client → server)
// PONG <version>|<server_name>|<online>/<max>|<pass_required 0/1>[|<name_color>]
std::string make_pong(const std::string& version, const std::string& server_name,
                      int online, int max_conn, bool pass_required,
                      const std::string& name_color = "");
bool parse_pong(const std::string& line, std::string& version, std::string& server_name,
                int& online, int& max_conn, bool& pass_required,
                std::string& name_color);

// ---- Encrypted session ----

// WELCOME <server_name> <0|1>
std::string make_welcome(const std::string& server_name, bool pass_required);
bool parse_welcome(const std::string& line, std::string& server_name, bool& pass_required);

// AUTH <user> <pass|-> [#RRGGBB] [key] [vN]
std::string make_auth(const std::string& user, const std::string& pass,
                      const std::string& color_hex, const std::string& admin_key,
                      const std::string& version = PROTO_VERSION);
bool parse_auth(const std::string& line, std::string& user, std::string& pass,
                std::string& color, bool& has_color,
                std::string& admin_key, bool& has_key,
                std::string& version);

// MSG <text>  (client → server, wraps user input)
std::string make_msg(const std::string& text);
bool parse_msg(const std::string& line, std::string& text);

// CHAT <iso> <name> <color|-> <role|-> <text...>
// role: - / admin / mod / sadmin / smod  (sadmin/smod = stealthed, only visible to staff)
std::string make_chat(const std::string& iso, const std::string& name,
                      const std::string& color, const std::string& role,
                      const std::string& text);
bool parse_chat(const std::string& line, std::string& iso, std::string& name,
                std::string& color, std::string& role, std::string& text);

// ACTION <iso> <name> <color|-> <role|-> <text...>  (/me)
std::string make_action(const std::string& iso, const std::string& name,
                        const std::string& color, const std::string& role,
                        const std::string& text);
bool parse_action(const std::string& line, std::string& iso, std::string& name,
                  std::string& color, std::string& role, std::string& text);

// DM <iso> <from> <from_color|-> <role|-> <to> <text...>
std::string make_dm(const std::string& iso, const std::string& from,
                    const std::string& color, const std::string& role,
                    const std::string& to, const std::string& text);
bool parse_dm(const std::string& line, std::string& iso, std::string& from,
              std::string& color, std::string& role, std::string& to, std::string& text);

// RENAME <old_name> <new_name>
std::string make_rename(const std::string& old_name, const std::string& new_name);
bool parse_rename(const std::string& line, std::string& old_name, std::string& new_name);

// MOTD <text>
std::string make_motd(const std::string& text);
bool parse_motd(const std::string& line, std::string& text);

// NOTICE <text>  (server notice, shown dim)
std::string make_notice(const std::string& text);
bool parse_notice(const std::string& line, std::string& text);

// HISTORY_START <count> / HISTORY_END
std::string make_history_start(int count);
std::string make_history_end();

// ADMIN_GRANTED <key>  — server notifies client they have been made admin
std::string make_admin_granted(const std::string& key);
bool parse_admin_granted(const std::string& line, std::string& key);

// MOD_GRANTED <key>  — server notifies client they have been made mod
std::string make_mod_granted(const std::string& key);
bool parse_mod_granted(const std::string& line, std::string& key);

// PING_USER  — server requests latency probe from a client
// PONG_USER  — client responds immediately
inline std::string make_ping_user() { return "PING_USER"; }
inline std::string make_pong_user() { return "PONG_USER"; }

} // namespace proto
