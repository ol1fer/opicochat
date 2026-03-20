#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "protocol.hpp"
#include "utils.hpp"

#ifndef _WIN32
  #include <signal.h>
  #include <sys/select.h>
  #include <termios.h>
  #include <unistd.h>
#else
  #include <conio.h>
  #include <winsock2.h>
#endif

using clk = std::chrono::steady_clock;

// ============================================================
// Console line editing
// ============================================================

static std::mutex     g_io_mtx;
static std::string    g_input_buf;

static void redraw_prompt() {
    std::cout << "\r\033[2K> " << g_input_buf << std::flush;
}
static void srv_print(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_io_mtx);
    std::cout << "\r\033[2K" << line << "\n";
    redraw_prompt();
}

#ifndef _WIN32
struct TermRawGuard {
    termios old{}; bool active = false;
    void enable() {
        if(active) return;
        tcgetattr(STDIN_FILENO, &old);
        termios raw = old; raw.c_lflag &= ~(uint32_t)(ICANON | ECHO);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw); active = true;
    }
    void disable() {
        if(!active) return;
        tcsetattr(STDIN_FILENO, TCSANOW, &old); active = false;
    }
    ~TermRawGuard() { disable(); }
};
#endif

// ============================================================
// Role badge helpers (ANSI)
// ============================================================

static std::string admin_badge()        { return "\033[32ma\033[0m"; }  // green
static std::string admin_stealth_badge(){ return "\033[90ma\033[0m"; }  // grey (stealth)
static std::string mod_badge()          { return "\033[34mm\033[0m"; }  // blue

// ============================================================
// Staff key databases (admins.cfg / mods.cfg)
// Format: key original_user last_user stealth_chat stealth_list
// ============================================================

struct StaffEntry {
    std::string original_user;
    std::string last_user;
    bool stealth_chat = false;
    bool stealth_list = false;
};

static std::unordered_map<std::string, StaffEntry> g_admins;
static std::unordered_map<std::string, StaffEntry> g_mods;

static std::string random_key(size_t n = 32) {
    static const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 61);
    std::string s; s.reserve(n);
    for(size_t i = 0; i < n; ++i) s += alpha[dist(rng)];
    return s;
}

static void load_staff(const std::string& path, std::unordered_map<std::string, StaffEntry>& out) {
    out.clear();
    std::ifstream f(path); if(!f.good()) return;
    std::string line;
    while(std::getline(f, line)) {
        line = trim(line); if(line.empty() || line[0] == '#') continue;
        std::istringstream ss(line); std::string key, orig, last, sc, sl;
        if(!(ss >> key >> orig >> last)) continue;
        StaffEntry e; e.original_user = orig; e.last_user = last;
        if(ss >> sc) e.stealth_chat = (sc == "1");
        if(ss >> sl) e.stealth_list = (sl == "1");
        out[key] = e;
    }
}
static void save_staff(const std::string& path,
                       const std::unordered_map<std::string, StaffEntry>& m) {
    std::ofstream f(path);
    for(auto& kv : m)
        f << kv.first << " " << kv.second.original_user << " " << kv.second.last_user
          << " " << (kv.second.stealth_chat ? "1" : "0")
          << " " << (kv.second.stealth_list ? "1" : "0") << "\n";
}

static std::unordered_map<std::string,std::string> g_ban_ips;

static void load_bans(const std::string& path) {
    g_ban_ips.clear();
    std::ifstream f(path); if(!f.good()) return;
    std::string line;
    while(std::getline(f, line)) {
        line = trim(line); if(line.empty() || line[0] == '#') continue;
        auto p = line.find('='); if(p == std::string::npos) continue;
        std::string k = line.substr(0, p), v = line.substr(p + 1);
        if(k.rfind("ip:", 0) == 0) g_ban_ips[k.substr(3)] = v;
    }
}
static void save_bans(const std::string& path) {
    std::ofstream f(path);
    for(auto& kv : g_ban_ips) f << "ip:" << kv.first << "=" << kv.second << "\n";
}

// ============================================================
// Client state
// ============================================================

struct MsgRateEntry {
    std::deque<clk::time_point> times;
    bool muted = false;
    clk::time_point mute_until{};
};

struct ClientInfo {
    socket_t             s{};
    std::string          ip;
    std::string          username;
    std::string          color_hex;
    std::string          staff_key;        // admin or mod key in use
    std::string          version;
    bool                 authed         = false;
    bool                 handshake_done = false;
    bool                 is_admin       = false;
    bool                 is_mod         = false;
    bool                 stealth_chat   = false;
    bool                 stealth_list   = false;
    bool                 was_kicked     = false;
    bool                 was_banned     = false;
    crypto::KeyPair      kp{};
    crypto::CipherStream cipher;
    clk::time_point      connect_time   = clk::now();
    int                  last_ping_ms   = -1;
    // Keepalive state
    clk::time_point      next_keepalive{};     // when to send next keepalive PING_USER
    bool                 keepalive_pending = false;
    clk::time_point      keepalive_sent_at{};
    // Personal /ping state
    bool                 personal_ping_pending = false;
    clk::time_point      personal_ping_sent_at{};
};

// ============================================================
// Helpers
// ============================================================

static bool file_exists(const std::string& p) { std::ifstream f(p); return f.good(); }

static std::string unique_log_path(const std::string& dir, const std::string& date) {
    std::string base = dir + "/" + date + ".log";
    if(!file_exists(base)) return base;
    for(int i = 1; i < 10000; ++i) {
        std::string p = dir + "/" + date + "-" + std::to_string(i) + ".log";
        if(!file_exists(p)) return p;
    }
    return base;
}

// Compute effective wire role for a client (handles stealth)
static std::string effective_role(const ClientInfo& c) {
    if(c.is_admin) return c.stealth_chat ? "sadmin" : "admin";
    if(c.is_mod)   return "mod";  // mods cannot stealth
    return "-";
}

static std::string pretty_chat(const std::string& raw_line) {
    std::string iso, name, color, role, text;
    if(proto::parse_chat(raw_line, iso, name, color, role, text)) {
        std::string ts = format_ts_prefix(iso);
        std::string badge;
        if(role == "admin" || role == "sadmin") badge = admin_badge() + " ";
        else if(role == "mod" || role == "smod") badge = mod_badge() + " ";
        return ts + badge + colorize_name(name, color == "-" ? "" : color) + " " + text;
    }
    std::string iso2, name2, color2, role2, text2;
    if(proto::parse_action(raw_line, iso2, name2, color2, role2, text2)) {
        std::string ts = format_ts_prefix(iso2);
        return ts + ansi_italic() + "* " + name2 + " " + text2 + ansi_reset();
    }
    return raw_line;
}

// Parse mute duration: 30s, 5m, 1h — default unit is minutes
static int parse_mute_duration(const std::string& s) {
    if(s.empty()) return 60; // default 1 minute
    try {
        if(s.back() == 's') return std::stoi(s.substr(0, s.size()-1));
        if(s.back() == 'm') return std::stoi(s.substr(0, s.size()-1)) * 60;
        if(s.back() == 'h') return std::stoi(s.substr(0, s.size()-1)) * 3600;
        return std::stoi(s) * 60; // no suffix = minutes
    } catch(...) { return 60; }
}

static std::string format_duration(int secs) {
    if(secs < 60)   return std::to_string(secs) + "s";
    if(secs < 3600) return std::to_string(secs/60) + "m";
    return std::to_string(secs/3600) + "h";
}

// (keepalive ping state is per-ClientInfo; no global PingRequest needed)

// ============================================================
// main
// ============================================================

int main(int argc, char** argv) {
#ifdef _WIN32
    enable_windows_ansi();
#else
    if(ensure_terminal_attached(argc, argv)) return 0;
    signal(SIGPIPE, SIG_IGN);
#endif

    auto cfg = ServerConfig::load_or_create("opicochatserver.cfg");
    ensure_dir(cfg.log_dir);
    load_staff("admins.cfg", g_admins);
    load_staff("mods.cfg",   g_mods);
    load_bans("banned.cfg");

    std::string err;
    socket_t lsock = net::listen_tcp(cfg.port, err);
    if(lsock == INVALID_SOCKET) { std::cerr << "error: " << err << "\n"; return 1; }
    std::cout << "opicochat v" << APP_VERSION << " (proto v" << PROTO_VERSION << ") "
              << "listening on port " << cfg.port
              << "  (logs: " << cfg.log_dir << ")\n"
              << "type /help for commands\n";

    std::string cur_date  = today_date();
    std::string cur_log   = unique_log_path(cfg.log_dir, cur_date);
    RingBuffer  history((size_t)std::max(0, cfg.history_size));

    append_file(cur_log, now_iso() + " [start] opicochat v" + APP_VERSION
                        + " (proto v" + PROTO_VERSION + ") port=" + std::to_string(cfg.port) + "\n");

    std::vector<ClientInfo> clients;
    clients.reserve(64);

    const clk::time_point server_start = clk::now();
    long total_msgs_sent = 0;  // chat messages broadcast (not system messages)

    std::unordered_map<std::string, std::deque<clk::time_point>> ip_rate;
    std::unordered_map<std::string, MsgRateEntry> msg_rate;
    std::string server_motd = cfg.motd;
    std::atomic<bool> running{true};
    int keepalive_stagger_idx = 0;
    bool lockdown = false;  // when true, new connections are rejected
    std::unordered_map<std::string, clk::time_point> ip_blocked; // IP -> unblock time

    std::mutex cmd_mtx;
    std::deque<std::function<void()>> cmd_q;
    auto post = [&](std::function<void()> fn) {
        std::lock_guard<std::mutex> lk(cmd_mtx);
        cmd_q.push_back(std::move(fn));
    };

    auto log_cmd = [&](const std::string& who, const std::string& raw) {
        append_file(cur_log, now_iso() + " [cmd] " + who + ": " + raw + "\n");
    };

    auto send_to = [&](ClientInfo& c, const std::string& line) {
        net::send_line(c.s, line, &c.cipher);
    };
    auto broadcast = [&](const std::string& line, ClientInfo* skip = nullptr) {
        for(auto& c : clients)
            if(c.authed && &c != skip) net::send_line(c.s, line, &c.cipher);
    };
    auto broadcast_and_log = [&](const std::string& line, ClientInfo* skip = nullptr) {
        broadcast(line, skip);
        append_file(cur_log, line + "\n");
        history.push(line);
        srv_print(pretty_chat(line));
    };
    auto send_notice = [&](ClientInfo& c, const std::string& text) {
        send_to(c, proto::make_notice(text));
    };

    // elevate a connected client to admin or mod, notifying them
    auto elevate_to_admin = [&](ClientInfo& c, const std::string& key) {
        c.is_admin = true; c.is_mod = false; c.staff_key = key;
        auto it = g_admins.find(key);
        if(it != g_admins.end()) {
            c.stealth_chat = it->second.stealth_chat;
            c.stealth_list = it->second.stealth_list;
            it->second.last_user = c.username;
            save_staff("admins.cfg", g_admins);
        }
        send_to(c, proto::make_admin_granted(key));
    };
    auto elevate_to_mod = [&](ClientInfo& c, const std::string& key) {
        c.is_mod = true; c.is_admin = false; c.staff_key = key;
        auto it = g_mods.find(key);
        if(it != g_mods.end()) {
            c.stealth_chat = it->second.stealth_chat;
            c.stealth_list = it->second.stealth_list;
            it->second.last_user = c.username;
            save_staff("mods.cfg", g_mods);
        }
        send_to(c, proto::make_mod_granted(key));
    };

    // -------------------------------------------------------
    // Command dispatcher
    // -------------------------------------------------------
    auto handle_cmd = [&](const std::string& slash, ClientInfo* from,
                          std::istringstream& ss) {
        auto next = [&]() { std::string t; ss >> t; return t; };
        auto rest = [&]() { std::string t; std::getline(ss, t); return trim(t); };
        auto srv_say = [&](const std::string& msg) {
            if(from) send_notice(*from, msg);
            else     srv_print(msg);
        };
        bool is_admin = from ? from->is_admin : true;
        bool is_staff = from ? (from->is_admin || from->is_mod) : true;

        if(slash == "/help") {
            const std::string G = "\033[32m", R = "\033[0m"; // green / reset
            // Base commands — visible to everyone
            std::vector<std::string> lines = {
                "/help               - show this",
                "/list [ping]        - who's online (ping shows last latency)",
                "/me <text>          - action message",
                "/dm <user> <text>   - direct message  (/msg alias)",
                "/nick <newname>     - change nickname",
                "/color <#hex>       - change color",
                "/ping               - check your latency to the server",
                "/motd               - show motd",
                "/serverinfo         - show server info",
                "/dc                 - disconnect",
            };
            if(is_staff) {
                lines.push_back(G+"/kick <user> [reason]"+R);
                lines.push_back(G+"/ban <user> [reason]   /banip <ip> [reason]"+R);
                lines.push_back(G+"/unban <ip>   /banlist"+R);
                lines.push_back(G+"/mute <user> [30s/5m/1h]   /unmute <user>"+R);
                lines.push_back(G+"/inspect <user>   /alist"+R);
                lines.push_back(G+"/announce <text>"+R);
                lines.push_back(G+"/lockdown on|off       - block new connections"+R);
                lines.push_back(G+"/health                - server health metrics"+R);
            }
            if(is_admin) {
                lines.push_back(G+"/stealth [chat|list] on|off"+R);
                lines.push_back(G+"/motd <text>           - set motd"+R);
                lines.push_back(G+"/admin add|remove|list <user>"+R);
                lines.push_back(G+"/mod add|remove|list <user>"+R);
                lines.push_back(G+"/reload bans|admins|mods"+R);
            }
            if(!from) lines.push_back("/say <text>  /shutdown  /status");
            for(auto& l : lines) srv_say(l);
            return;
        }

        if(slash == "/list") {
            std::string sub = next();
            bool viewer_is_admin = from ? from->is_admin : true;
            // Returns the appropriate badge for a client as seen by this viewer.
            // Stealth admins (sadmin) are only visible to other admins, not mods.
            auto list_badge = [&](const ClientInfo& c) -> std::string {
                if(c.is_admin) {
                    if(c.stealth_list) return viewer_is_admin ? (admin_stealth_badge()+" ") : "";
                    return admin_badge() + " ";
                }
                if(c.is_mod) return mod_badge() + " ";
                return "";
            };
            if(sub == "ping") {
                int cnt = 0;
                for(auto& c : clients) {
                    if(!c.authed) continue;
                    ++cnt;
                    std::string ping_s = c.last_ping_ms >= 0 ? std::to_string(c.last_ping_ms)+"ms" : "?";
                    srv_say(list_badge(c) + colorize_name(c.username, c.color_hex) + ansi_reset() + "  " + ping_s);
                }
                if(cnt == 0) srv_say("no users online.");
            } else {
                std::string names; int cnt = 0;
                for(auto& c : clients) {
                    if(!c.authed) continue;
                    if(cnt++) names += ", ";
                    names += list_badge(c) + colorize_name(c.username, c.color_hex) + ansi_reset();
                }
                srv_say("online (" + std::to_string(cnt) + "): " + (names.empty() ? "(none)" : names));
            }
            return;
        }

        // /ping — reply with the user's last known latency (populated by keepalive)
        if(slash == "/ping") {
            if(!from) { srv_say("(server has no latency)"); return; }
            if(from->personal_ping_pending) { srv_say("ping already in flight."); return; }
            from->personal_ping_pending = true;
            from->personal_ping_sent_at = clk::now();
            send_to(*from, proto::make_ping_user());
            return;
        }

        // /alist — staff only, shows IPs and stealth indicators
        if(slash == "/alist") {
            int cnt = 0;
            for(auto& c : clients) {
                if(!c.authed) continue;
                ++cnt;
                std::string badge;
                if(c.is_admin) badge = (c.stealth_list ? admin_stealth_badge() : admin_badge()) + " ";
                else if(c.is_mod) badge = mod_badge() + " ";
                else badge = "  ";
                std::string ping_s = c.last_ping_ms >= 0 ? (std::to_string(c.last_ping_ms)+"ms") : "?";
                std::string stealth_note = (c.is_admin && (c.stealth_chat || c.stealth_list))
                    ? " \033[90m[stealth: "
                        + std::string(c.stealth_chat ? "chat" : "")
                        + std::string(c.stealth_chat && c.stealth_list ? "+" : "")
                        + std::string(c.stealth_list ? "list" : "")
                        + "]\033[0m"
                    : "";
                srv_say(badge + colorize_name(c.username, c.color_hex) + ansi_reset()
                    + "  " + c.ip + "  " + ping_s + stealth_note);
            }
            if(cnt == 0) srv_say("no users online.");
            return;
        }

        // /health — staff only, server health metrics
        if(slash == "/health") {
            auto now = clk::now();
            // Uptime
            auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(now - server_start).count();
            long uh = uptime_s/3600, um = (uptime_s%3600)/60, us = uptime_s%60;
            char upbuf[32]; std::snprintf(upbuf, sizeof(upbuf), "%ldh %ldm %lds", uh, um, us);
            // Counts
            int authed_clients = 0, handshaking = 0, keepalive_pending_cnt = 0;
            int admin_online = 0, mod_online = 0, muted_cnt = 0;
            for(auto& c : clients) {
                if(c.authed) {
                    ++authed_clients;
                    if(c.is_admin) ++admin_online;
                    if(c.is_mod)   ++mod_online;
                    if(c.keepalive_pending) ++keepalive_pending_cnt;
                } else {
                    ++handshaking;
                }
            }
            for(auto& kv : msg_rate) if(kv.second.muted) ++muted_cnt;
            // Recent connection rate (connections in last minute across all IPs)
            int recent_conns = 0;
            auto one_min_ago = now - std::chrono::minutes(1);
            for(auto& kv : ip_rate)
                for(auto& t : kv.second) if(t > one_min_ago) ++recent_conns;
            // History
            int hist_used = (int)history.snapshot().size();
            srv_say("--- server health ---");
            srv_say("uptime:          " + std::string(upbuf));
            srv_say("connections:     " + std::to_string(authed_clients) + " authed"
                + "  " + std::to_string(handshaking) + " handshaking"
                + "  / " + std::to_string(cfg.max_total_conn) + " max");
            srv_say("staff online:    " + std::to_string(admin_online) + " admin  "
                + std::to_string(mod_online) + " mod");
            srv_say("registered:      " + std::to_string(g_admins.size()) + " admins  "
                + std::to_string(g_mods.size()) + " mods");
            srv_say("lockdown:        " + std::string(lockdown ? "\033[31mON\033[0m" : "off"));
            srv_say("banned IPs:      " + std::to_string(g_ban_ips.size()));
            srv_say("temp-blocked IPs:" + std::to_string(ip_blocked.size()));
            srv_say("muted users:     " + std::to_string(muted_cnt));
            srv_say("keepalive wait:  " + std::to_string(keepalive_pending_cnt) + " pending pong");
            srv_say("conn rate/min:   " + std::to_string(recent_conns) + " (limit "
                + std::to_string(cfg.max_conn_rate_per_min) + "/ip)");
            srv_say("IPs tracked:     " + std::to_string(ip_rate.size()));
            srv_say("msg history:     " + std::to_string(hist_used) + " / "
                + std::to_string(cfg.history_size));
            srv_say("total msgs:      " + std::to_string(total_msgs_sent));
            srv_say("keepalive:       every " + std::to_string(cfg.keepalive_interval_secs)
                + "s  timeout " + std::to_string(cfg.keepalive_timeout_secs) + "s");
            srv_say("log:             " + cur_log);
            srv_say("proto version:   " + std::string(PROTO_VERSION));
            return;
        }

        // /serverinfo — any user
        if(slash == "/serverinfo") {
            int online = 0; for(auto& c : clients) if(c.authed) ++online;
            srv_say("server: " + cfg.server_name);
            srv_say("slots:  " + std::to_string(online) + "/" + std::to_string(cfg.max_total_conn));
            srv_say("motd:   " + (server_motd.empty() ? "(none)" : server_motd));
            return;
        }

        // Staff-only block (mod or admin)
        static const std::set<std::string> staff_cmds = {
            "/kick","/ban","/banip","/unban","/banlist",
            "/mute","/unmute","/inspect","/announce","/health","/alist","/lockdown"
        };
        if(!is_staff && staff_cmds.count(slash)) { srv_say("permission denied."); return; }

        // Admin-only block
        static const std::set<std::string> admin_cmds = {
            "/admin","/mod","/reload","/stealth"
        };
        if(!is_admin && admin_cmds.count(slash)) { srv_say("permission denied."); return; }
        if(!is_admin && slash == "/motd" && from) {
            // non-admin viewing /motd is handled in client-command section
        }

        if(slash == "/lockdown") {
            std::string arg = next();
            if(arg == "on") {
                lockdown = true;
                srv_say("lockdown enabled — new connections blocked.");
                broadcast(proto::make_notice("[server] lockdown enabled — no new users may join."));
            } else if(arg == "off") {
                lockdown = false;
                srv_say("lockdown disabled — new connections allowed.");
                broadcast(proto::make_notice("[server] lockdown lifted."));
            } else {
                srv_say("lockdown is currently " + std::string(lockdown ? "ON" : "OFF")
                    + ".  usage: /lockdown on|off");
            }
            return;
        }

        if(slash == "/kick") {
            std::string user = next();
            std::string reason = rest(); if(reason.empty()) reason = "you were kicked.";
            if(user.empty()) { srv_say("usage: /kick <user> [reason]"); return; }
            bool found = false;
            for(auto& c : clients) {
                if(!c.authed || c.username != user) continue;
                // Mods cannot kick admins
                if(from && from->is_mod && !from->is_admin && c.is_admin) {
                    srv_say("mods cannot kick admins."); return;
                }
                c.was_kicked = true;
                send_to(c, "ERROR " + reason);
#ifdef _WIN32
                ::shutdown(c.s, SD_BOTH);
#else
                ::shutdown(c.s, SHUT_RDWR);
#endif
                found = true; break;
            }
            srv_say(found ? ("kicked " + user) : ("no such user: " + user));
            return;
        }

        if(slash == "/ban") {
            std::string user = next();
            std::string reason = rest(); if(reason.empty()) reason = "you are banned.";
            if(user.empty()) { srv_say("usage: /ban <user> [reason]"); return; }
            std::string ip; bool found = false;
            for(auto& c : clients) {
                if(!c.authed || c.username != user) continue;
                if(from && from->is_mod && !from->is_admin && c.is_admin) {
                    srv_say("mods cannot ban admins."); return;
                }
                ip = c.ip; c.was_banned = true;
                send_to(c, "ERROR " + reason);
#ifdef _WIN32
                ::shutdown(c.s, SD_BOTH);
#else
                ::shutdown(c.s, SHUT_RDWR);
#endif
                found = true; break;
            }
            if(found) { g_ban_ips[ip] = reason; save_bans("banned.cfg"); srv_say("banned " + user + " (ip " + ip + ")"); }
            else srv_say("no such user: " + user);
            return;
        }

        if(slash == "/banip") {
            std::string ip = next();
            std::string reason = rest(); if(reason.empty()) reason = "you are banned.";
            if(ip.empty()) { srv_say("usage: /banip <ip> [reason]"); return; }
            g_ban_ips[ip] = reason; save_bans("banned.cfg");
            for(auto& c : clients) {
                if(c.ip == ip) {
                    c.was_banned = true; send_to(c, "ERROR " + reason);
#ifdef _WIN32
                    ::shutdown(c.s, SD_BOTH);
#else
                    ::shutdown(c.s, SHUT_RDWR);
#endif
                }
            }
            srv_say("banned ip " + ip);
            return;
        }

        if(slash == "/unban") {
            std::string ip = next(); if(ip.empty()) { srv_say("usage: /unban <ip>"); return; }
            size_t n = g_ban_ips.erase(ip); save_bans("banned.cfg");
            srv_say(n ? ("unbanned " + ip) : ("ip not found: " + ip));
            return;
        }

        if(slash == "/banlist") {
            if(g_ban_ips.empty()) { srv_say("no banned ips."); return; }
            for(auto& kv : g_ban_ips) srv_say("  " + kv.first + " -> " + kv.second);
            return;
        }

        if(slash == "/mute") {
            std::string user = next(); std::string dur_s = next();
            if(user.empty()) { srv_say("usage: /mute <user> [30s|5m|1h]"); return; }
            int secs = parse_mute_duration(dur_s);
            bool found = false;
            for(auto& c : clients) {
                if(!c.authed || c.username != user) continue;
                auto& r = msg_rate[user];
                r.muted = true; r.mute_until = clk::now() + std::chrono::seconds(secs);
                send_notice(c, "you have been muted for " + format_duration(secs) + ".");
                found = true; break;
            }
            srv_say(found ? ("muted " + user + " for " + format_duration(secs))
                          : ("no such user: " + user));
            return;
        }

        if(slash == "/unmute") {
            std::string user = next(); if(user.empty()) { srv_say("usage: /unmute <user>"); return; }
            auto it = msg_rate.find(user);
            if(it != msg_rate.end()) it->second.muted = false;
            for(auto& c : clients) if(c.authed && c.username == user) send_notice(c, "you have been unmuted.");
            srv_say("unmuted " + user);
            return;
        }

        if(slash == "/inspect") {
            std::string user = next(); if(user.empty()) { srv_say("usage: /inspect <user>"); return; }
            bool found = false;
            for(auto& c : clients) {
                if(!c.authed || c.username != user) continue;
                auto now = clk::now();
                auto total_secs = std::chrono::duration_cast<std::chrono::seconds>(now - c.connect_time).count();
                std::string uptime;
                if(total_secs < 60) uptime = std::to_string(total_secs) + "s";
                else if(total_secs < 3600) uptime = std::to_string(total_secs/60) + "m " + std::to_string(total_secs%60) + "s";
                else uptime = std::to_string(total_secs/3600) + "h " + std::to_string((total_secs%3600)/60) + "m";

                std::string role = c.is_admin ? "admin" : c.is_mod ? "mod" : "user";
                std::string ping_s = c.last_ping_ms >= 0 ? std::to_string(c.last_ping_ms)+"ms" : "unknown";

                // mute status
                std::string mute_s = "no";
                auto mit = msg_rate.find(user);
                if(mit != msg_rate.end() && mit->second.muted) {
                    auto secs_left = std::chrono::duration_cast<std::chrono::seconds>(mit->second.mute_until - now).count();
                    mute_s = secs_left > 0 ? "yes (" + std::to_string(secs_left) + "s remaining)" : "yes (expired)";
                }

                // stealth status (only show to admins)
                std::string stealth_s = "n/a";
                if(c.is_admin) {
                    if(c.stealth_chat && c.stealth_list) stealth_s = "chat+list";
                    else if(c.stealth_chat) stealth_s = "chat only";
                    else if(c.stealth_list) stealth_s = "list only";
                    else stealth_s = "off";
                }

                srv_say("-- " + user + " --");
                srv_say("  ip:      " + c.ip);
                srv_say("  role:    " + role);
                srv_say("  version: " + (c.version.empty() ? "unknown" : "v"+c.version));
                srv_say("  color:   " + (c.color_hex.empty() ? "(auto)" : c.color_hex));
                srv_say("  online:  " + uptime);
                srv_say("  ping:    " + ping_s);
                srv_say("  muted:   " + mute_s);
                if(c.is_admin) srv_say("  stealth: " + stealth_s);
                found = true; break;
            }
            if(!found) srv_say("no such user: " + user);
            return;
        }

        if(slash == "/stealth") {
            if(!from) { srv_say("console is always visible."); return; }
            std::string arg1 = next(); // "on", "off", "chat", "list"
            std::string arg2 = next(); // "on" / "off" (if arg1 is chat/list)
            bool set_chat, set_list;
            bool value;
            if(arg1 == "chat" || arg1 == "list") {
                if(arg2 != "on" && arg2 != "off") { srv_say("usage: /stealth [chat|list] on|off"); return; }
                value = (arg2 == "on");
                set_chat = (arg1 == "chat");
                set_list = (arg1 == "list");
            } else if(arg1 == "on" || arg1 == "off") {
                value = (arg1 == "on");
                set_chat = set_list = true;
            } else {
                srv_say("usage: /stealth [chat|list] on|off"); return;
            }
            if(set_chat) from->stealth_chat = value;
            if(set_list) from->stealth_list = value;
            // Persist
            if(!from->staff_key.empty()) {
                auto it_a = g_admins.find(from->staff_key);
                if(it_a != g_admins.end()) {
                    if(set_chat) it_a->second.stealth_chat = value;
                    if(set_list) it_a->second.stealth_list = value;
                    save_staff("admins.cfg", g_admins);
                }
                auto it_m = g_mods.find(from->staff_key);
                if(it_m != g_mods.end()) {
                    if(set_chat) it_m->second.stealth_chat = value;
                    if(set_list) it_m->second.stealth_list = value;
                    save_staff("mods.cfg", g_mods);
                }
            }
            srv_say("stealth" + std::string(set_chat && set_list ? "" : (set_chat ? " chat" : " list"))
                    + (value ? " on." : " off."));
            return;
        }

        if(slash == "/announce") {
            std::string text = rest(); if(text.empty()) { srv_say("usage: /announce <text>"); return; }
            std::string ann = proto::make_chat(now_iso(), "[server]", "-", "-",
                                               ansi_bold() + "** " + text + " **" + ansi_reset());
            broadcast_and_log(ann);
            return;
        }

        if(slash == "/motd") {
            std::string text = rest();
            if(text.empty()) {
                srv_say(server_motd.empty() ? "no motd set." : "motd: " + server_motd);
            } else {
                if(!is_admin) { srv_say("permission denied."); return; }
                server_motd = text; cfg.motd = text;
                broadcast(proto::make_motd(text));
                append_file(cur_log, now_iso() + " [motd] " + text + "\n");
                srv_say("motd set.");
            }
            return;
        }

        if(slash == "/admin") {
            std::string sub = next();
            if(sub == "add") {
                std::string user = next(); if(user.empty()) { srv_say("usage: /admin add <user>"); return; }
                std::string key = random_key(32);
                g_admins[key] = {user, user, false, false};
                save_staff("admins.cfg", g_admins);
                for(auto& c : clients)
                    if(c.authed && c.username == user)
                        elevate_to_admin(c, key);
                srv_say("admin key for " + user + ": " + key
                    + (from ? " (sent to them if online)" : ""));
            } else if(sub == "remove") {
                std::string user = next(); if(user.empty()) { srv_say("usage: /admin remove <user>"); return; }
                std::string rk;
                for(auto& c : clients)
                    if(c.authed && c.username == user && !c.staff_key.empty()) { rk = c.staff_key; break; }
                if(rk.empty())
                    for(auto& kv : g_admins)
                        if(kv.second.original_user==user||kv.second.last_user==user) { rk=kv.first; break; }
                if(rk.empty()) { srv_say("no admin key for " + user); return; }
                g_admins.erase(rk); save_staff("admins.cfg", g_admins);
                for(auto& c : clients) if(c.authed && c.staff_key == rk) { c.is_admin=false; c.staff_key=""; }
                srv_say("removed admin for " + user);
            } else if(sub == "list") {
                if(g_admins.empty()) { srv_say("no admins."); return; }
                for(auto& kv : g_admins)
                    srv_say("  " + kv.second.original_user + " (last: " + kv.second.last_user + ")");
            } else { srv_say("usage: /admin add|remove|list [user]"); }
            return;
        }

        if(slash == "/mod") {
            std::string sub = next();
            if(sub == "add") {
                std::string user = next(); if(user.empty()) { srv_say("usage: /mod add <user>"); return; }
                std::string key = random_key(32);
                g_mods[key] = {user, user, false, false};
                save_staff("mods.cfg", g_mods);
                for(auto& c : clients)
                    if(c.authed && c.username == user)
                        elevate_to_mod(c, key);
                srv_say("mod key for " + user + ": " + key);
            } else if(sub == "remove") {
                std::string user = next(); if(user.empty()) { srv_say("usage: /mod remove <user>"); return; }
                std::string rk;
                for(auto& c : clients)
                    if(c.authed && c.username == user && !c.staff_key.empty() && c.is_mod) { rk=c.staff_key; break; }
                if(rk.empty())
                    for(auto& kv : g_mods)
                        if(kv.second.original_user==user||kv.second.last_user==user) { rk=kv.first; break; }
                if(rk.empty()) { srv_say("no mod key for " + user); return; }
                g_mods.erase(rk); save_staff("mods.cfg", g_mods);
                for(auto& c : clients) if(c.authed && c.staff_key == rk) { c.is_mod=false; c.staff_key=""; }
                srv_say("removed mod for " + user);
            } else if(sub == "list") {
                if(g_mods.empty()) { srv_say("no mods."); return; }
                for(auto& kv : g_mods)
                    srv_say("  " + kv.second.original_user + " (last: " + kv.second.last_user + ")");
            } else { srv_say("usage: /mod add|remove|list [user]"); }
            return;
        }

        if(slash == "/reload") {
            std::string what = next();
            if(what == "bans")        { load_bans("banned.cfg");           srv_say("reloaded bans."); }
            else if(what == "admins") { load_staff("admins.cfg", g_admins); srv_say("reloaded admins."); }
            else if(what == "mods")   { load_staff("mods.cfg", g_mods);    srv_say("reloaded mods."); }
            else srv_say("usage: /reload bans|admins|mods");
            return;
        }

        // Console-only
        if(!from) {
            if(slash == "/say") {
                std::string text = rest();
                if(!text.empty()) broadcast_and_log(
                    proto::make_chat(now_iso(), "[server]", "-", "-", sanitize_message(text)));
                return;
            }
            if(slash == "/shutdown" || slash == "/stop") {
                srv_print("shutting down..."); running = false; return;
            }
            if(slash == "/status") {
                int cnt = 0; for(auto& c : clients) if(c.authed) ++cnt;
                srv_say(std::to_string(cnt) + " user(s) online, "
                    + std::to_string(clients.size()) + " connection(s).");
                return;
            }
        }

        srv_say("unknown command: " + slash);
    };

    // -------------------------------------------------------
    // Console thread
    // -------------------------------------------------------
    std::thread console([&] {
        std::vector<std::string> hist; int hist_idx = -1; std::string saved;
#ifndef _WIN32
        TermRawGuard rg; rg.enable();
        char esc_buf[8] = {}; int esc_len = 0; bool in_esc = false;
        (void)esc_buf;
#endif
        for(;;) {
            if(!running) break;
#ifdef _WIN32
            if(!_kbhit()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
            int ch = _getch();
            if(ch == 0 || ch == 0xE0) {
                int scan = _getch();
                std::lock_guard<std::mutex> lk(g_io_mtx);
                if(scan == 72) {
                    if(!hist.empty()) {
                        if(hist_idx < 0) { saved = g_input_buf; hist_idx = (int)hist.size()-1; }
                        else if(hist_idx > 0) --hist_idx;
                        g_input_buf = hist[hist_idx]; redraw_prompt();
                    }
                } else if(scan == 80) {
                    if(hist_idx >= 0) {
                        ++hist_idx;
                        if(hist_idx >= (int)hist.size()) { hist_idx=-1; g_input_buf=saved; }
                        else g_input_buf = hist[hist_idx];
                        redraw_prompt();
                    }
                }
                continue;
            }
            if(ch == '\r' || ch == '\n') {
                std::string raw;
                { std::lock_guard<std::mutex> lk(g_io_mtx);
                  raw=trim(g_input_buf); g_input_buf.clear(); hist_idx=-1; redraw_prompt(); }
                if(raw.empty()) continue;
                if(hist.empty()||hist.back()!=raw) hist.push_back(raw);
                if(hist.size()>100) hist.erase(hist.begin());
                if(raw[0]=='/') {
                    post([&,raw]{ log_cmd("[console]",raw);
                        std::istringstream ss(raw); std::string slash; ss>>slash;
                        handle_cmd(slash,nullptr,ss); });
                } else {
                    post([&,raw]{ broadcast_and_log(
                        proto::make_chat(now_iso(),"[server]","-","-",sanitize_message(raw))); });
                }
            } else if(ch==8||ch==127) {
                std::lock_guard<std::mutex> lk(g_io_mtx);
                if(!g_input_buf.empty()) g_input_buf.pop_back(); redraw_prompt();
            } else if(ch==3) { running=false; break; }
            else if(ch>=32&&ch<127) {
                std::lock_guard<std::mutex> lk(g_io_mtx);
                g_input_buf.push_back((char)ch); redraw_prompt();
            }
#else
            fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO,&rf);
            timeval tv{0,100000};
            int sel=select(STDIN_FILENO+1,&rf,nullptr,nullptr,&tv);
            if(!running) break; if(sel<=0) continue;
            unsigned char ch=0;
            ssize_t n=read(STDIN_FILENO,&ch,1); if(n<=0) continue;
            if(in_esc) {
                esc_buf[esc_len++]=(char)ch;
                if(esc_len==1&&ch=='[') { }
                else if(esc_len==2) {
                    in_esc=false; esc_len=0;
                    std::lock_guard<std::mutex> lk(g_io_mtx);
                    if(ch=='A') {
                        if(!hist.empty()) {
                            if(hist_idx<0){saved=g_input_buf;hist_idx=(int)hist.size()-1;}
                            else if(hist_idx>0) --hist_idx;
                            g_input_buf=hist[hist_idx]; redraw_prompt();
                        }
                    } else if(ch=='B') {
                        if(hist_idx>=0) {
                            ++hist_idx;
                            if(hist_idx>=(int)hist.size()){hist_idx=-1;g_input_buf=saved;}
                            else g_input_buf=hist[hist_idx];
                            redraw_prompt();
                        }
                    }
                } else { in_esc=false; esc_len=0; }
                continue;
            }
            if(ch==0x1b){in_esc=true;esc_len=0;continue;}
            if(ch=='\r'||ch=='\n') {
                std::string raw;
                { std::lock_guard<std::mutex> lk(g_io_mtx);
                  raw=trim(g_input_buf); g_input_buf.clear(); hist_idx=-1; redraw_prompt(); }
                if(raw.empty()) continue;
                if(hist.empty()||hist.back()!=raw) hist.push_back(raw);
                if(hist.size()>100) hist.erase(hist.begin());
                if(raw[0]=='/') {
                    post([&,raw]{ log_cmd("[console]",raw);
                        std::istringstream ss(raw); std::string slash; ss>>slash;
                        handle_cmd(slash,nullptr,ss); });
                } else {
                    post([&,raw]{ broadcast_and_log(
                        proto::make_chat(now_iso(),"[server]","-","-",sanitize_message(raw))); });
                }
            } else if(ch==127||ch==8) {
                std::lock_guard<std::mutex> lk(g_io_mtx);
                if(!g_input_buf.empty()) g_input_buf.pop_back(); redraw_prompt();
            } else if(ch==3){running=false;break;}
            else if(ch==12){
                std::lock_guard<std::mutex> lk(g_io_mtx);
                std::cout<<"\033[2J\033[H"; redraw_prompt();
            } else if(ch>=32) {
                std::lock_guard<std::mutex> lk(g_io_mtx);
                g_input_buf.push_back((char)ch); redraw_prompt();
            }
#endif
        }
    });

    // -------------------------------------------------------
    // Main select loop
    // -------------------------------------------------------
    while(running) {
        // Rotate log on day change
        { std::string today=today_date();
          if(today!=cur_date){cur_date=today;cur_log=unique_log_path(cfg.log_dir,today);} }

        // Drain console command queue
        { std::deque<std::function<void()>> tmp;
          { std::lock_guard<std::mutex> lk(cmd_mtx); tmp.swap(cmd_q); }
          for(auto& fn:tmp) fn(); }

        // Keepalive: send PING_USER to each client on schedule; kick if no PONG_USER in time
        {
            auto now = clk::now();
            for(auto& c : clients) {
                if(!c.authed) continue;
                if(c.keepalive_pending) {
                    // Check timeout
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - c.keepalive_sent_at).count();
                    if(elapsed >= cfg.keepalive_timeout_secs) {
                        // No response — kick silently
                        c.was_kicked = true;
                    }
                } else if(now >= c.next_keepalive) {
                    send_to(c, proto::make_ping_user());
                    c.keepalive_pending  = true;
                    c.keepalive_sent_at  = now;
                    c.next_keepalive     = now + std::chrono::seconds(cfg.keepalive_interval_secs);
                }
            }
        }

        fd_set rf; FD_ZERO(&rf); FD_SET(lsock,&rf);
        socket_t maxfd=lsock;
        for(auto& c:clients){FD_SET(c.s,&rf);if(c.s>maxfd)maxfd=c.s;}
        timeval tv{0,50*1000};
        int r=select((int)(maxfd+1),&rf,nullptr,nullptr,&tv);
        if(r<0) continue;

        // Accept
        if(FD_ISSET(lsock,&rf)) {
            sockaddr_storage ss{}; socklen_t slen=sizeof(ss);
            socket_t cs=accept(lsock,(sockaddr*)&ss,&slen);
            if(cs!=INVALID_SOCKET) {
                char host[NI_MAXHOST]; std::string ip;
                if(getnameinfo((sockaddr*)&ss,slen,host,sizeof(host),nullptr,0,NI_NUMERICHOST)==0)
                    ip=host;
                if(lockdown) {
                    net::send_line(cs,"ERROR server is in lockdown, try again later");
                    closesocket_cross(cs); goto after_accept;
                }
                if((int)clients.size()>=cfg.max_total_conn) {
                    net::send_line(cs,"ERROR server full"); closesocket_cross(cs); goto after_accept;
                }
                // Check temporary IP block (from rate-limit enforcement)
                { auto now=clk::now();
                  auto bit=ip_blocked.find(ip);
                  if(bit!=ip_blocked.end()) {
                      if(now < bit->second) {
                          auto secs=std::chrono::duration_cast<std::chrono::seconds>(bit->second-now).count();
                          net::send_line(cs,"ERROR rate limited, try again in "+std::to_string(secs)+"s");
                          closesocket_cross(cs); goto after_accept;
                      } else {
                          ip_blocked.erase(bit); // block expired
                      }
                  }
                }
                { int conc=0; for(auto& u:clients) if(u.ip==ip) ++conc;
                  if(conc>=cfg.max_conn_per_ip) {
                      net::send_line(cs,"ERROR too many connections from your ip");
                      closesocket_cross(cs); goto after_accept; } }
                { auto& dq=ip_rate[ip]; auto now=clk::now();
                  while(!dq.empty()&&(now-dq.front())>std::chrono::minutes(1)) dq.pop_front();
                  if((int)dq.size()>=cfg.max_conn_rate_per_min) {
                      // Enforce block for configured duration
                      ip_blocked[ip] = now + std::chrono::seconds(cfg.ip_block_duration_secs);
                      srv_print("[rate-limit] blocked " + ip + " for "
                          + std::to_string(cfg.ip_block_duration_secs) + "s");
                      net::send_line(cs,"ERROR rate limited, blocked for "
                          +std::to_string(cfg.ip_block_duration_secs)+"s");
                      closesocket_cross(cs); goto after_accept; }
                  dq.push_back(now); }
                {
                    ClientInfo ci; ci.s=cs; ci.ip=ip; ci.connect_time=clk::now();
                    ci.kp=crypto::keygen();
                    // Stagger keepalive: spread pings by 150ms per client slot to avoid thundering herd
                    int stagger_ms = (keepalive_stagger_idx++ % std::max(1, cfg.max_total_conn)) * 150;
                    ci.next_keepalive = clk::now()
                        + std::chrono::seconds(cfg.keepalive_interval_secs)
                        + std::chrono::milliseconds(stagger_ms);
                    net::send_line(ci.s, proto::make_hello(crypto::to_hex(ci.kp.pub,32)));
                    clients.push_back(std::move(ci));
                }
            }
        }
        after_accept:;

        // Read from clients
        for(size_t i=0; i<clients.size();) {
            auto& c=clients[i]; bool disc=false; std::string line;
            if(!FD_ISSET(c.s,&rf)){++i;continue;}

            while(net::recv_line(c.s,line,0,disc,&c.cipher)) {

                // ---- DH handshake ----
                if(!c.handshake_done) {
                    std::string peer_hex;
                    if(line=="PING") {
                        int online=0; for(auto& u:clients) if(u.authed) ++online;
                        net::send_line(c.s, proto::make_pong(PROTO_VERSION,cfg.server_name,
                            online,cfg.max_total_conn,cfg.password_enabled,cfg.server_name_color));
                        disc=true; break;
                    } else if(proto::parse_hello(line,peer_hex)) {
                        uint8_t peer_pub[32];
                        if(!crypto::from_hex(peer_hex,peer_pub,32)){disc=true;break;}
                        uint8_t shared[32];
                        crypto::exchange(shared,c.kp.priv,peer_pub);
                        c.cipher.init(shared); c.handshake_done=true;
                        send_to(c, proto::make_welcome(cfg.server_name,cfg.password_enabled));
                    } else { disc=true; break; }
                    continue;
                }

                // ---- AUTH ----
                if(!c.authed) {
                    std::string user,pass,color,key,version;
                    bool has_color=false,has_key=false;
                    if(!proto::parse_auth(line,user,pass,color,has_color,key,has_key,version)) {
                        send_to(c,"ERROR bad auth format"); disc=true; break;
                    }
                    user=trim(user);
                    if(!is_valid_username(user)) {
                        send_to(c,"AUTH_FAIL invalid username"); disc=true; break;
                    }
                    if(is_reserved_name(user)) {
                        send_to(c,"AUTH_FAIL reserved username"); disc=true; break;
                    }
                    if(auto it=g_ban_ips.find(c.ip);it!=g_ban_ips.end()) {
                        send_to(c,"ERROR "+it->second); disc=true; break;
                    }
                    bool dup=false;
                    for(auto& u:clients) if(&u!=&c&&u.authed&&u.username==user){dup=true;break;}
                    if(dup){send_to(c,"AUTH_FAIL username already in use");disc=true;break;}
                    if(cfg.password_enabled&&pass!=cfg.password) {
                        send_to(c,"AUTH_FAIL wrong password"); disc=true; break;
                    }

                    if(has_color) c.color_hex=normalize_hex_hash(color);
                    c.username=user; c.version=version; c.authed=true;

                    // Check admin key
                    if(has_key) {
                        if(auto it=g_admins.find(key);it!=g_admins.end()) {
                            c.is_admin=true; c.staff_key=key;
                            c.stealth_chat=it->second.stealth_chat;
                            c.stealth_list=it->second.stealth_list;
                            it->second.last_user=user; save_staff("admins.cfg",g_admins);
                        } else if(auto it2=g_mods.find(key);it2!=g_mods.end()) {
                            c.is_mod=true; c.staff_key=key;
                            c.stealth_chat=it2->second.stealth_chat;
                            c.stealth_list=it2->second.stealth_list;
                            it2->second.last_user=user; save_staff("mods.cfg",g_mods);
                        }
                    }

                    std::string role_tag = c.is_admin ? " admin" : c.is_mod ? " mod" : "";
                    send_to(c, "AUTH_OK" + role_tag);

                    if(!version.empty() && version != APP_VERSION) {
                        if(!cfg.allow_version_mismatch) {
                            send_to(c, "AUTH_FAIL version mismatch: server v"
                                +std::string(APP_VERSION)+", client v"+version);
                            disc=true; break;
                        }
                        send_notice(c,"warning: version mismatch (server v"
                            +std::string(APP_VERSION)+", you v"+version+")");
                    }

                    int online=0; for(auto& u:clients) if(u.authed) ++online;
                    send_notice(c,std::to_string(online)+(online==1?" user":" users")+" online");
                    if(!server_motd.empty()) send_to(c,proto::make_motd(server_motd));

                    auto snap=history.snapshot();
                    if(!snap.empty()) {
                        send_to(c,proto::make_history_start((int)snap.size()));
                        for(auto& h:snap) send_to(c,h);
                        send_to(c,proto::make_history_end());
                    }

                    srv_print("[+] "+user+" from "+c.ip
                        +(c.is_admin?" (admin)":c.is_mod?" (mod)":"")
                        +(!version.empty()?" v"+version:""));
                    append_file(cur_log,now_iso()+" [join] "+user+" from "+c.ip+"\n");

                    std::string jmsg=proto::make_chat(now_iso(),"[server]","-","-",user+" joined");
                    broadcast(jmsg);
                    append_file(cur_log,jmsg+"\n"); history.push(jmsg);
                    break;
                }

                // ---- Authenticated client messages ----

                // PONG_USER (response to PING_USER — keepalive or /ping)
                if(line=="PONG_USER") {
                    auto now = clk::now();
                    if(c.personal_ping_pending) {
                        // /ping response: calculate RTT and notify user
                        auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - c.personal_ping_sent_at).count();
                        c.last_ping_ms = (int)rtt;
                        c.personal_ping_pending = false;
                        send_notice(c, "pong! (" + std::to_string(rtt) + "ms)");
                    } else if(c.keepalive_pending) {
                        auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - c.keepalive_sent_at).count();
                        c.last_ping_ms = (int)rtt;
                        c.keepalive_pending = false;
                    }
                    continue;
                }

                if(line=="QUIT"){disc=true;break;}

                if(line.rfind("MSG ",0)!=0) continue;
                std::string text; if(!proto::parse_msg(line,text)) continue;
                text=sanitize_message(text); if(text.empty()) continue;

                // Mute check
                { auto& rt=msg_rate[c.username];
                  if(rt.muted) {
                      if(clk::now()>=rt.mute_until) rt.muted=false;
                      else{send_notice(c,"you are muted.");continue;}
                  } }

                if(text[0]=='/') {
                    log_cmd(c.username,text);
                    srv_print("[cmd] "+c.username+": "+text);
                    std::istringstream ss(text); std::string slash; ss>>slash;

                    if(slash=="/me") {
                        std::string rest; std::getline(ss,rest); rest=trim(rest);
                        if(rest.empty()) continue;
                        broadcast_and_log(proto::make_action(
                            now_iso(),c.username,c.color_hex,effective_role(c),rest));
                        continue;
                    }

                    if(slash=="/dm"||slash=="/msg"||slash=="/w") {
                        std::string target; ss>>target;
                        std::string dmtext; std::getline(ss,dmtext); dmtext=trim(dmtext);
                        if(target.empty()||dmtext.empty()){send_notice(c,"usage: /dm <user> <text>");continue;}
                        ClientInfo* tgt=nullptr;
                        for(auto& u:clients) if(u.authed&&u.username==target){tgt=&u;break;}
                        if(!tgt){send_notice(c,"no such user: "+target);continue;}
                        std::string dm=proto::make_dm(now_iso(),c.username,c.color_hex,effective_role(c),target,dmtext);
                        send_to(*tgt,dm); send_to(c,dm);
                        append_file(cur_log,dm+"\n");
                        continue;
                    }

                    if(slash=="/nick") {
                        std::string newname; ss>>newname; newname=trim(newname);
                        if(newname.empty()){send_notice(c,"usage: /nick <newname>");continue;}
                        if(!is_valid_username(newname)||is_reserved_name(newname)){
                            send_notice(c,"invalid username.");continue;}
                        bool taken=false;
                        for(auto& u:clients) if(&u!=&c&&u.authed&&u.username==newname){taken=true;break;}
                        if(taken){send_notice(c,"username already in use.");continue;}
                        std::string old=c.username;
                        auto node=msg_rate.extract(old);
                        c.username=newname;
                        if(!node.empty()){node.key()=newname;msg_rate.insert(std::move(node));}
                        if(!c.staff_key.empty()) {
                            auto it_a=g_admins.find(c.staff_key);
                            if(it_a!=g_admins.end()){it_a->second.last_user=newname;save_staff("admins.cfg",g_admins);}
                            auto it_m=g_mods.find(c.staff_key);
                            if(it_m!=g_mods.end()){it_m->second.last_user=newname;save_staff("mods.cfg",g_mods);}
                        }
                        broadcast_and_log(proto::make_rename(old,newname));
                        append_file(cur_log,now_iso()+" [rename] "+old+" -> "+newname+"\n");
                        continue;
                    }

                    if(slash=="/color") {
                        std::string hex; ss>>hex; if(hex.empty()){send_notice(c,"usage: /color <#RRGGBB>");continue;}
                        std::string norm=normalize_hex_hash(hex);
                        if(norm.empty()){send_notice(c,"invalid color.");continue;}
                        c.color_hex=norm; send_notice(c,"color updated."); continue;
                    }

                    if(slash=="/motd") {
                        // non-admin just views
                        if(!c.is_admin) {
                            if(server_motd.empty()) send_notice(c,"no motd set.");
                            else send_to(c,proto::make_motd(server_motd));
                            continue;
                        }
                    }

                    // /adminkey elevate <key>
                    if(slash=="/adminkey") {
                        std::string sub2; ss>>sub2;
                        if(sub2=="elevate") {
                            std::string ekey; ss>>ekey;
                            if(ekey.empty()){send_notice(c,"usage: /adminkey elevate <key>");continue;}
                            if(auto it=g_admins.find(ekey);it!=g_admins.end()) {
                                elevate_to_admin(c,ekey);
                                send_notice(c,"elevated to admin! type /adminkey add to save your key.");
                            } else if(auto it2=g_mods.find(ekey);it2!=g_mods.end()) {
                                elevate_to_mod(c,ekey);
                                send_notice(c,"elevated to mod! type /adminkey add to save your key.");
                            } else {
                                send_notice(c,"invalid key.");
                            }
                        } else {
                            send_notice(c,"/adminkey elevate <key> — handled client-side: add/show");
                        }
                        continue;
                    }

                    if(slash=="/dc"||slash=="/disconnect"){disc=true;break;}

                    handle_cmd(slash,&c,ss);
                    continue;
                }

                // Message rate limit
                { auto& rt=msg_rate[c.username]; auto now=clk::now();
                  auto window=std::chrono::seconds(cfg.msg_window_secs);
                  while(!rt.times.empty()&&(now-rt.times.front())>window) rt.times.pop_front();
                  if((int)rt.times.size()>=cfg.max_msg_per_window){send_notice(c,"slow down!");continue;}
                  rt.times.push_back(now); }

                broadcast_and_log(proto::make_chat(now_iso(),c.username,
                    c.color_hex.empty()?"-":c.color_hex, effective_role(c), text));
                ++total_msgs_sent;
            }

            if(disc) {
                net::clear_buffer(c.s); closesocket_cross(c.s);
                msg_rate.erase(c.username);
                if(c.authed) {
                    const char* action=c.was_banned?" was banned":c.was_kicked?" was kicked":" left";
                    std::string leave=proto::make_chat(now_iso(),"[server]","-","-",c.username+action);
                    for(size_t j=0;j<clients.size();++j)
                        if(j!=i&&clients[j].authed)
                            net::send_line(clients[j].s,leave,&clients[j].cipher);
                    append_file(cur_log,leave+"\n"); history.push(leave);
                    srv_print("[-] "+c.username+action);
                }
                clients.erase(clients.begin()+(ptrdiff_t)i);
                continue;
            }
            ++i;
        }
    }

    if(console.joinable()) console.join();
    for(auto& c:clients){net::clear_buffer(c.s);closesocket_cross(c.s);}
    closesocket_cross(lsock);
    std::cout<<"\nserver stopped.\n";
    return 0;
}
