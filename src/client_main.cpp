#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "protocol.hpp"
#include "utils.hpp"

#ifdef _WIN32
  #include <conio.h>
  #include <windows.h>
#else
  #include <unistd.h>
  #include <termios.h>
  #include <sys/select.h>
#endif

using namespace std::chrono;

static const char* GITHUB_RELEASES_API = "https://api.github.com/repos/ol1fer/opicochat/releases/latest";
#ifdef _WIN32
  static const char* CLIENT_UPDATE_ASSET = "opicochat-windows-x64.exe";
#elif defined(__APPLE__)
  static const char* CLIENT_UPDATE_ASSET = "opicochat-macos-arm64";
#else
  static const char* CLIENT_UPDATE_ASSET = "opicochat-linux-x64";
#endif

// ============================================================
// Config helpers
// ============================================================

static std::string hp_key(const std::string& host, uint16_t port) {
    return host + ":" + std::to_string(port);
}

static void save_cfg(const ClientConfig& cc) {
    Ini ini = ClientConfig::to_ini(cc);
    std::ofstream f("opicochat.cfg");
    f << ini.serialize();
}

// ============================================================
// Terminal raw mode (Linux/macOS)
// ============================================================

#ifndef _WIN32
struct TermRawGuard {
    termios old{}; bool active = false;
    void enable() {
        if(active) return;
        tcgetattr(STDIN_FILENO, &old);
        termios raw = old;
        raw.c_lflag &= ~(uint32_t)(ICANON | ECHO);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        active = true;
    }
    void disable() {
        if(!active) return;
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        active = false;
    }
    ~TermRawGuard() { disable(); }
};
#endif

// ============================================================
// Input line editor state
// ============================================================

struct InputState {
    std::string buf;
    size_t      cursor   = 0;
    int         hist_idx = -1;
    std::string saved_buf;
    std::vector<std::string> history;
};

static void redraw_input(const InputState& in) {
    std::string line = "> " + in.buf;
    size_t col = 2 + in.cursor;
    std::cout << "\r\033[2K" << line
              << "\r\033[" << (col + 1) << "C" << std::flush;
}

// ============================================================
// Server ping (unencrypted — before DH handshake)
// ============================================================

struct PingResult {
    bool ok          = false;
    std::string name;
    std::string name_color;   // from server config, may be empty
    int  online      = 0;
    int  max_conn    = 0;
    bool pass_req    = false;
    long latency_ms  = -1;
};

static PingResult ping_server(const std::string& host, uint16_t port) {
    PingResult res;
    std::string err;
    socket_t s = net::connect_tcp(host, port, err);
    if(s == INVALID_SOCKET) return res;

    auto t0 = steady_clock::now();

    bool disc = false; std::string line;
    auto deadline = steady_clock::now() + seconds(4);
    bool got_hello = false;
    while(steady_clock::now() < deadline) {
        if(net::recv_line(s, line, 200, disc)) {
            std::string pub_hex;
            if(proto::parse_hello(line, pub_hex)) { got_hello = true; break; }
        }
        if(disc) break;
    }
    if(!got_hello) { closesocket_cross(s); return res; }

    net::send_line(s, "PING");

    deadline = steady_clock::now() + seconds(4);
    while(steady_clock::now() < deadline) {
        if(net::recv_line(s, line, 200, disc)) {
            std::string ver, name;
            int on = 0, mx = 0; bool pw = false;
            std::string name_col;
            if(proto::parse_pong(line, ver, name, on, mx, pw, name_col)) {
                res.latency_ms  = duration_cast<milliseconds>(steady_clock::now() - t0).count();
                res.ok          = true;
                res.name        = name;
                res.name_color  = name_col;
                res.online      = on;
                res.max_conn    = mx;
                res.pass_req    = pw;
                break;
            }
        }
        if(disc) break;
    }
    closesocket_cross(s);
    return res;
}

// ============================================================
// Badge helpers
// ============================================================

// Returns a role badge prefix for display, respecting stealth visibility.
// sadmin = stealthed admin: grey 'a' visible only to OTHER ADMINS (not mods).
static std::string role_badge(const std::string& role, bool viewer_is_admin) {
    if(role == "admin")  return "\033[32ma\033[0m ";                          // green a
    if(role == "mod")    return "\033[34mm\033[0m ";                          // blue m
    if(role == "sadmin") return viewer_is_admin ? "\033[90ma\033[0m " : "";   // grey a, admins only
    if(role == "smod")   return "\033[34mm\033[0m ";                          // blue m (no mod stealth)
    return "";
}

// ============================================================
// Message rendering
// ============================================================

static std::string render_line(const std::string& l,
                               const std::string& my_name,
                               bool show_ts,
                               bool my_is_admin,
                               const std::set<std::string>& ignored) {
    std::string iso, name, color, role, text;

    if(proto::parse_chat(l, iso, name, color, role, text)) {
        if(!ignored.empty() && ignored.count(name)) return "";
        std::string ts    = show_ts ? format_ts_prefix(iso) : "";
        std::string badge = role_badge(role, my_is_admin);
        std::string col   = colorize_name(name, color == "-" ? "" : color);
        return ts + badge + col + " " + text;
    }

    if(proto::parse_action(l, iso, name, color, role, text)) {
        if(!ignored.empty() && ignored.count(name)) return "";
        std::string ts = show_ts ? format_ts_prefix(iso) : "";
        return ts + ansi_italic() + "* " + name + " " + text + ansi_reset();
    }

    {
        std::string from, fcol, frole, to, dmtext;
        if(proto::parse_dm(l, iso, from, fcol, frole, to, dmtext)) {
            if(!ignored.empty() && ignored.count(from)) return "";
            std::string ts = show_ts ? format_ts_prefix(iso) : "";
            bool outgoing = (from == my_name);
            std::string arrow = outgoing
                ? (ansi_dim()  + "[dm -> "   + to   + "]" + ansi_reset())
                : (ansi_bold() + "[dm from " + from + "]" + ansi_reset());
            return ts + arrow + " " + dmtext;
        }
    }

    {
        std::string old_n, new_n;
        if(proto::parse_rename(l, old_n, new_n)) {
            return ansi_dim() + "* " + old_n + " is now known as " + new_n + ansi_reset();
        }
    }

    {
        std::string motd_text;
        if(proto::parse_motd(l, motd_text)) {
            return ansi_dim() + "~ motd: " + motd_text + " ~" + ansi_reset();
        }
    }

    {
        std::string notice_text;
        if(proto::parse_notice(l, notice_text)) {
            return ansi_dim() + "  " + notice_text + ansi_reset();
        }
    }

    if(l.rfind("AUTH_OK", 0) == 0)      return "";
    if(l == "HISTORY_END")               return "";
    if(l.rfind("HISTORY_START", 0) == 0) return "";

    return l;
}

// Strip ANSI escape sequences for plain-text log output
static std::string strip_ansi(const std::string& s) {
    std::string out; out.reserve(s.size());
    for(size_t i = 0; i < s.size(); ++i) {
        if(s[i] == '\033' && i+1 < s.size() && s[i+1] == '[') {
            i += 2;
            while(i < s.size() && s[i] != 'm') ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

// ============================================================
// Chat session
// ============================================================

// saved_username_idx: index into cfg.usernames of the selected username (-1 = typed manually)
static bool run_chat(const std::string& host, uint16_t port,
                     const std::string& username,
                     const std::string& color_hex,
                     const std::string& admin_key,
                     const std::string& stored_password,
                     int  saved_username_idx,
                     bool& show_ts,
                     std::set<std::string>& ignored,
                     ClientConfig& cfg)
{
    std::string err;
    socket_t s = net::connect_tcp(host, port, err);
    if(s == INVALID_SOCKET) {
        std::cout << "error: " << err << "\n";
        return false;
    }

    crypto::CipherStream cipher;
    bool disc = false; std::string line;

    // Self-update state
    std::string cli_update_version;
    std::string cli_update_url;

    // Client-side logging setup (used throughout run_chat)
    bool session_log = cfg.client_log_enabled;
    ensure_dir("logs");
    std::string client_log_path = "logs/client-" + today_date() + "-" + host + ".log";

    // 1. DH handshake: wait for server HELLO, respond with our HELLO
    {
        auto deadline = steady_clock::now() + seconds(8);
        bool got = false;
        while(steady_clock::now() < deadline) {
            if(net::recv_line(s, line, 300, disc)) {
                std::string srv_pub_hex;
                if(proto::parse_hello(line, srv_pub_hex)) {
                    uint8_t srv_pub[32];
                    if(!crypto::from_hex(srv_pub_hex, srv_pub, 32)) {
                        std::cout << "error: bad server public key\n";
                        closesocket_cross(s); return false;
                    }
                    auto kp = crypto::keygen();
                    net::send_line(s, proto::make_hello(crypto::to_hex(kp.pub, 32)));
                    uint8_t shared[32];
                    crypto::exchange(shared, kp.priv, srv_pub);
                    cipher.init(shared);
                    got = true;
                    break;
                }
            }
            if(disc) break;
        }
        if(!got || disc) {
            std::cout << "error: no response from server\n";
            closesocket_cross(s); return false;
        }
    }

    // 2. Wait for WELCOME (encrypted)
    std::string password_to_use = stored_password;
    {
        auto deadline = steady_clock::now() + seconds(8);
        bool got = false;
        while(steady_clock::now() < deadline) {
            if(net::recv_line(s, line, 300, disc, &cipher)) {
                std::string sname; bool pass_req;
                if(proto::parse_welcome(line, sname, pass_req)) {
                    got = true;
                    if(pass_req && password_to_use.empty()) {
                        std::cout << "server password: ";
                        std::getline(std::cin, password_to_use);
                        password_to_use = trim(password_to_use);
                    }
                    break;
                }
            }
            if(disc) break;
        }
        if(!got || disc) {
            std::cout << "error: no welcome from server\n";
            closesocket_cross(s); return false;
        }
    }

    // 3. Send AUTH (encrypted)
    net::send_line(s, proto::make_auth(username, password_to_use, color_hex, admin_key), &cipher);

    // 4. Wait for AUTH_OK or error
    std::string my_role = "-";
    {
        bool got_ok = false;
        auto deadline = steady_clock::now() + seconds(8);
        while(steady_clock::now() < deadline) {
            if(net::recv_line(s, line, 300, disc, &cipher)) {
                if(line.rfind("AUTH_OK", 0) == 0) {
                    got_ok = true;
                    if(line.find("admin") != std::string::npos) my_role = "admin";
                    else if(line.find("mod") != std::string::npos) my_role = "mod";
                    break;
                }
                if(line.rfind("AUTH_FAIL", 0) == 0) {
                    if(line.find("wrong password") != std::string::npos) {
                        std::cout << "incorrect password.\n";
                        std::string hpk = hp_key(host, port);
                        if(!stored_password.empty()) {
                            cfg.server_passwords.erase(hpk);
                            save_cfg(cfg);
                        }
                    } else {
                        std::cout << (line.size() > 10 ? line.substr(10) : line) << "\n";
                    }
                    closesocket_cross(s); return false;
                }
                if(line.rfind("ERROR ", 0) == 0) {
                    std::cout << line.substr(6) << "\n";
                    closesocket_cross(s); return false;
                }
            }
            if(disc) break;
        }
        if(!got_ok) {
            std::cout << "error: auth failed or server unreachable\n";
            closesocket_cross(s); return false;
        }
        std::cout << "connected as " << colorize_name(username, color_hex) << ansi_reset();
        if(my_role == "admin") std::cout << " (admin)";
        else if(my_role == "mod") std::cout << " (mod)";
        std::cout << "\ntype /help for commands, /dc to disconnect\n";
        if(session_log)
            append_file(client_log_path, now_iso() + " [session] connected to "
                        + host + ":" + std::to_string(port) + " as " + username + "\n");
    }

    // Hand off the recv buffer to the RX thread
    std::string leftover = net::take_recv_buffer(s);

    std::atomic<bool> running{true};
    std::mutex io_mtx;
    InputState in_state;

    // Session state (protected by io_mtx where shared with RX thread)
    std::string my_name  = username;
    std::string my_color = color_hex;
    std::string my_key   = admin_key;
    // my_role already set

    std::mutex q_mtx;
    std::condition_variable q_cv;
    std::deque<std::string> q;

    bool in_history = false;

    // Pause system (protected by io_mtx)
    static constexpr size_t PAUSE_BUF_MAX = 500;
    bool paused = false;
    std::deque<std::string> pause_buf;
    size_t pause_dropped = 0;

    auto cli_print = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lk(io_mtx);
        std::cout << "\r\033[2K" << msg << "\n";
        if(!in_history) redraw_input(in_state);
    };

    // ---- RX thread ----
    std::thread rx([&] {
        net::seed_recv_buffer(s, std::move(leftover));
        std::string l; bool d = false;
        while(running) {
            if(!net::recv_line(s, l, 500, d, &cipher)) {
                if(d) {
                    cli_print("disconnected.");
                    running = false; q_cv.notify_all();
                }
                continue;
            }

            // Respond to latency probes immediately from RX thread (concurrent write is safe)
            if(l == "PING_USER") {
                net::send_line(s, proto::make_pong_user(), &cipher);
                continue;
            }

            if(l.rfind("ERROR ", 0) == 0) {
                cli_print(l.substr(6));
                running = false; q_cv.notify_all(); return;
            }

            // Elevation notifications
            {
                std::string key;
                if(proto::parse_admin_granted(l, key)) {
                    { std::lock_guard<std::mutex> lk(io_mtx); my_role = "admin"; my_key = key; }
                    cli_print("\033[32m  you have been granted admin!\033[0m");
                    cli_print("  admin key: " + key);
                    cli_print("  type /adminkey add  to save it to this server entry");
                    continue;
                }
                if(proto::parse_mod_granted(l, key)) {
                    { std::lock_guard<std::mutex> lk(io_mtx); my_role = "mod"; my_key = key; }
                    cli_print("\033[34m  you have been granted mod!\033[0m");
                    cli_print("  mod key: " + key);
                    cli_print("  type /adminkey add  to save it to this server entry");
                    continue;
                }
            }

            if(l.rfind("HISTORY_START", 0) == 0) {
                in_history = true;
                std::lock_guard<std::mutex> lk(io_mtx);
                std::cout << "\r\033[2K" << ansi_dim() << "--- message history ---" << ansi_reset() << "\n";
                continue;
            }
            if(l == "HISTORY_END") {
                in_history = false;
                std::lock_guard<std::mutex> lk(io_mtx);
                std::cout << "\r\033[2K" << ansi_dim() << "--- ---" << ansi_reset() << "\n";
                redraw_input(in_state);
                continue;
            }

            {
                std::string on, nn;
                if(proto::parse_rename(l, on, nn)) {
                    std::lock_guard<std::mutex> lk(io_mtx);
                    if(on == my_name) my_name = nn;
                }
            }

            bool is_admin;
            { std::lock_guard<std::mutex> lk(io_mtx);
              is_admin = (my_role == "admin" || my_role == "sadmin"); }

            std::string rendered = render_line(l, my_name, show_ts, is_admin, ignored);
            if(!rendered.empty()) {
                std::lock_guard<std::mutex> lk(io_mtx);
                if(session_log)
                    append_file(client_log_path, strip_ansi(rendered) + "\n");
                if(paused) {
                    if(pause_buf.size() >= PAUSE_BUF_MAX) {
                        pause_buf.pop_front();
                        ++pause_dropped;
                    }
                    pause_buf.push_back(rendered);
                } else {
                    std::cout << "\r\033[2K" << rendered << "\n";
                    if(!in_history) redraw_input(in_state);
                }
            }
        }
    });

    // ---- Input thread ----
    std::thread input_thr([&] {
#ifdef _WIN32
        while(running) {
            if(!_kbhit()) { std::this_thread::sleep_for(milliseconds(10)); continue; }
            int ch = _getch();
            if(ch == 0 || ch == 0xE0) {
                int scan = _getch();
                std::lock_guard<std::mutex> lk(io_mtx);
                auto& in = in_state;
                if(scan == 72) {
                    if(!in.history.empty()) {
                        if(in.hist_idx < 0) { in.saved_buf = in.buf; in.hist_idx = (int)in.history.size()-1; }
                        else if(in.hist_idx > 0) --in.hist_idx;
                        in.buf = in.history[in.hist_idx]; in.cursor = in.buf.size();
                        redraw_input(in);
                    }
                } else if(scan == 80) {
                    if(in.hist_idx >= 0) {
                        ++in.hist_idx;
                        if(in.hist_idx >= (int)in.history.size()) { in.hist_idx = -1; in.buf = in.saved_buf; }
                        else in.buf = in.history[in.hist_idx];
                        in.cursor = in.buf.size(); redraw_input(in);
                    }
                } else if(scan == 75) { if(in.cursor > 0) { --in.cursor; redraw_input(in); } }
                else if(scan == 77) { if(in.cursor < in.buf.size()) { ++in.cursor; redraw_input(in); } }
                else if(scan == 71) { in.cursor = 0; redraw_input(in); }
                else if(scan == 79) { in.cursor = in.buf.size(); redraw_input(in); }
                else if(scan == 83) { if(in.cursor < in.buf.size()) { in.buf.erase(in.cursor, 1); redraw_input(in); } }
                continue;
            }
            if(ch == '\r' || ch == '\n') {
                std::string out;
                { std::lock_guard<std::mutex> lk(io_mtx);
                  out = in_state.buf; in_state.buf.clear(); in_state.cursor = 0;
                  in_state.hist_idx = -1; redraw_input(in_state); }
                { std::lock_guard<std::mutex> lk(q_mtx); q.push_back(out); }
                q_cv.notify_one();
            } else if(ch == 8) {
                std::lock_guard<std::mutex> lk(io_mtx);
                auto& in = in_state;
                if(in.cursor > 0) { in.buf.erase(--in.cursor, 1); redraw_input(in); }
            } else if(ch == 3) { running = false; q_cv.notify_all(); break; }
            else if(ch >= 32 && ch < 127) {
                std::lock_guard<std::mutex> lk(io_mtx);
                auto& in = in_state;
                in.buf.insert(in.cursor++, 1, (char)ch); redraw_input(in);
            }
        }
#else
        TermRawGuard rg; rg.enable();
        char esc_buf[8]; int esc_len = 0; bool in_esc = false;
        while(running) {
            fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
            timeval tv{0, 50000};
            int sel = select(STDIN_FILENO+1, &rf, nullptr, nullptr, &tv);
            if(!running) break;
            if(sel <= 0) continue;
            unsigned char ch = 0;
            if(read(STDIN_FILENO, &ch, 1) <= 0) continue;

            if(in_esc) {
                esc_buf[esc_len++] = (char)ch;
                if(esc_len == 1 && ch == '[') { /* CSI */ }
                else if(esc_len >= 2) {
                    in_esc = false;
                    std::lock_guard<std::mutex> lk(io_mtx);
                    auto& in = in_state;
                    if(esc_buf[0] == '[') {
                        if(ch == 'A') {
                            if(!in.history.empty()) {
                                if(in.hist_idx < 0) { in.saved_buf = in.buf; in.hist_idx = (int)in.history.size()-1; }
                                else if(in.hist_idx > 0) --in.hist_idx;
                                in.buf = in.history[in.hist_idx]; in.cursor = in.buf.size();
                                redraw_input(in);
                            }
                        } else if(ch == 'B') {
                            if(in.hist_idx >= 0) {
                                ++in.hist_idx;
                                if(in.hist_idx >= (int)in.history.size()) { in.hist_idx = -1; in.buf = in.saved_buf; }
                                else in.buf = in.history[in.hist_idx];
                                in.cursor = in.buf.size(); redraw_input(in);
                            }
                        } else if(ch == 'C') {
                            if(in.cursor < in.buf.size()) { ++in.cursor; redraw_input(in); }
                        } else if(ch == 'D') {
                            if(in.cursor > 0) { --in.cursor; redraw_input(in); }
                        } else if(esc_len >= 3 && esc_buf[1] == '3' && ch == '~') {
                            if(in.cursor < in.buf.size()) { in.buf.erase(in.cursor, 1); redraw_input(in); }
                        } else if(esc_len >= 3 && esc_buf[1] == '1' && ch == '~') {
                            in.cursor = 0; redraw_input(in);
                        } else if(esc_len >= 3 && esc_buf[1] == '4' && ch == '~') {
                            in.cursor = in.buf.size(); redraw_input(in);
                        } else if(ch == 'H') { in.cursor = 0; redraw_input(in); }
                        else if(ch == 'F') { in.cursor = in.buf.size(); redraw_input(in); }
                    }
                    esc_len = 0;
                }
                continue;
            }
            if(ch == 0x1b) { in_esc = true; esc_len = 0; continue; }

            if(ch == '\r' || ch == '\n') {
                std::string out;
                { std::lock_guard<std::mutex> lk(io_mtx);
                  out = in_state.buf; in_state.buf.clear(); in_state.cursor = 0;
                  in_state.hist_idx = -1; redraw_input(in_state); }
                { std::lock_guard<std::mutex> lk(q_mtx); q.push_back(out); }
                q_cv.notify_one();
            } else if(ch == 127 || ch == 8) {
                std::lock_guard<std::mutex> lk(io_mtx);
                auto& in = in_state;
                if(in.cursor > 0) { in.buf.erase(--in.cursor, 1); redraw_input(in); }
            } else if(ch == 3) { running = false; q_cv.notify_all(); break; }
            else if(ch == 12) {
                std::lock_guard<std::mutex> lk(io_mtx);
                std::cout << "\033[2J\033[H"; redraw_input(in_state);
            } else if(ch == 1) {
                std::lock_guard<std::mutex> lk(io_mtx);
                in_state.cursor = 0; redraw_input(in_state);
            } else if(ch == 5) {
                std::lock_guard<std::mutex> lk(io_mtx);
                in_state.cursor = in_state.buf.size(); redraw_input(in_state);
            } else if(ch >= 32) {
                std::lock_guard<std::mutex> lk(io_mtx);
                auto& in = in_state;
                in.buf.insert(in.cursor++, 1, (char)ch); redraw_input(in);
            }
        }
#endif
    });

    // ---- Sender loop ----
    bool want_reconnect = false;
    while(running) {
        std::unique_lock<std::mutex> lk(q_mtx);
        q_cv.wait(lk, [&]{ return !q.empty() || !running; });
        if(!running) break;
        std::string input = std::move(q.front()); q.pop_front(); lk.unlock();

        input = trim(input);
        if(input.empty()) continue;

        { std::lock_guard<std::mutex> lk2(io_mtx);
          if(in_state.history.empty() || in_state.history.back() != input)
              in_state.history.push_back(input);
          if(in_state.history.size() > 200) in_state.history.erase(in_state.history.begin()); }

        // ---- /pause / /play / /unpause ----
        if(input == "/pause") {
            std::lock_guard<std::mutex> lk2(io_mtx);
            if(paused) { std::cout << "\r\033[2K  already paused. type /play or /unpause to resume.\n"; }
            else { paused = true; pause_buf.clear(); pause_dropped = 0;
                   std::cout << "\r\033[2K  chat paused. type /play or /unpause to resume.\n"; }
            redraw_input(in_state); continue;
        }
        if(input == "/play" || input == "/unpause") {
            std::deque<std::string> to_show;
            size_t dropped;
            {
                std::lock_guard<std::mutex> lk2(io_mtx);
                if(!paused) { std::cout << "\r\033[2K  chat is not paused.\n"; redraw_input(in_state); continue; }
                paused = false;
                to_show  = std::move(pause_buf);
                dropped  = pause_dropped;
                pause_buf.clear(); pause_dropped = 0;
            }
            if(dropped > 0) {
                std::lock_guard<std::mutex> lk2(io_mtx);
                std::cout << "\r\033[2K  \033[33mwarning: " << dropped
                          << " messages were dropped (buffer limit " << PAUSE_BUF_MAX << ").\033[0m\n";
                redraw_input(in_state);
            }
            if(!to_show.empty()) {
                bool many = to_show.size() > 50;
                if(many) {
                    std::lock_guard<std::mutex> lk2(io_mtx);
                    std::cout << "\r\033[2K  unpausing — replaying " << to_show.size() << " messages...\n";
                    redraw_input(in_state);
                }
                for(auto& msg : to_show) {
                    std::lock_guard<std::mutex> lk2(io_mtx);
                    std::cout << "\r\033[2K" << msg << "\n";
                    redraw_input(in_state);
                }
                if(many) {
                    std::lock_guard<std::mutex> lk2(io_mtx);
                    std::cout << "\r\033[2K  -- end of buffered messages --\n";
                    redraw_input(in_state);
                }
            } else {
                std::lock_guard<std::mutex> lk2(io_mtx);
                std::cout << "\r\033[2K  chat resumed (no messages missed).\n";
                redraw_input(in_state);
            }
            continue;
        }

        // ---- /log ----
        if(input.rfind("/log", 0) == 0 && (input.size() == 4 || input[4] == ' ')) {
            std::string arg = trim(input.size() > 4 ? input.substr(4) : "");
            std::lock_guard<std::mutex> lk2(io_mtx);
            if(arg == "on") {
                session_log = true;
                std::cout << "\r\033[2K  logging on -> " << client_log_path << "\n";
            } else if(arg == "off") {
                session_log = false;
                std::cout << "\r\033[2K  logging off\n";
            } else {
                std::cout << "\r\033[2K  logging is " << (session_log ? "on" : "off")
                          << "  (log: " << client_log_path << ")\n";
                std::cout << "  /log on | off\n";
            }
            redraw_input(in_state); continue;
        }

        // ---- /updateclient ----
        if(input.rfind("/updateclient", 0) == 0 && (input.size() == 13 || input[13] == ' ')) {
            std::string arg = trim(input.size() > 13 ? input.substr(13) : "");
            bool is_force   = (arg == "force");
            bool is_confirm = (arg == "confirm");

            if(is_confirm || is_force) {
                std::string use_version = cli_update_version;
                std::string use_url     = cli_update_url;
                if(is_force || use_url.empty()) {
                    if(is_confirm && use_url.empty()) {
                        cli_print("  no update check done yet. run /updateclient first.");
                        continue;
                    }
                    // fetch now
                    cli_print("  checking for updates...");
                    std::string json = update_http_get(GITHUB_RELEASES_API);
                    if(json.empty()) { cli_print("  failed to reach github."); continue; }
                    std::string tag = update_get_latest_tag(json);
                    use_version = tag;
                    if(!use_version.empty() && use_version[0] == 'v') use_version = use_version.substr(1);
                    if(use_version.empty()) { cli_print("  could not parse response from github."); continue; }
                    use_url = update_find_asset_url(json, CLIENT_UPDATE_ASSET);
                    if(use_url.empty()) { cli_print("  no binary found for this platform."); continue; }
                }
                std::string exe = get_self_exe_path();
                if(exe.empty()) { cli_print("  could not determine client executable path."); continue; }
#ifdef _WIN32
                std::string bat = update_write_bat(exe, false);
                if(bat.empty()) { cli_print("  failed to write updater batch file."); continue; }
                update_launch_file(bat);
                cli_print("  updater launched. disconnecting...");
                net::send_line(s, "QUIT", &cipher);
                running = false; q_cv.notify_all();
                closesocket_cross(s);
                std::exit(0);
#else
                std::string tmp = exe + ".update";
                cli_print("  downloading v" + use_version + "...");
                if(!update_download_file(use_url, tmp)) {
                    cli_print("  download failed. check curl is available."); continue;
                }
                std::string msg;
                if(update_apply_binary(tmp, exe, msg)) {
                    cli_print("  " + msg + " relaunching...");
                    net::send_line(s, "QUIT", &cipher);
                    closesocket_cross(s);
                    execl(exe.c_str(), exe.c_str(), (char*)nullptr);
                    cli_print("  execl failed — restart manually.");
                } else {
                    cli_print("  update failed: " + msg);
                }
#endif
                cli_update_version.clear(); cli_update_url.clear();
            } else {
                cli_print("  checking for updates...");
                std::string json = update_http_get(GITHUB_RELEASES_API);
                if(json.empty()) {
                    cli_print("  failed to reach github. check curl is installed (linux/mac) or powershell is available (windows).");
                } else {
                    std::string tag = update_get_latest_tag(json);
                    std::string latest = tag;
                    if(!latest.empty() && latest[0] == 'v') latest = latest.substr(1);
                    if(latest.empty()) {
                        cli_print("  could not parse response from github.");
                    } else if(latest == APP_VERSION) {
                        cli_print("  already up to date (v" + std::string(APP_VERSION) + "). use /updateclient force to reinstall.");
                    } else {
                        std::string url = update_find_asset_url(json, CLIENT_UPDATE_ASSET);
                        if(url.empty()) {
                            cli_print("  update v" + latest + " found but no binary for this platform.");
                        } else {
                            cli_update_version = latest;
                            cli_update_url = url;
                            cli_print("  update available: v" + latest + " (current: v" + std::string(APP_VERSION) + ")");
                            cli_print("  type /updateclient confirm to apply.");
                        }
                    }
                }
            }
            continue;
        }

        if(input == "/dc" || input == "/disconnect") {
            net::send_line(s, "QUIT", &cipher); running = false; q_cv.notify_all(); break;
        }
        if(input == "/rc" || input == "/reconnect") {
            net::send_line(s, "QUIT", &cipher); want_reconnect = true; running = false; q_cv.notify_all(); break;
        }
        if(input == "/clear") {
            std::lock_guard<std::mutex> lk2(io_mtx);
            std::cout << "\033[2J\033[H"; redraw_input(in_state); continue;
        }
        if(input == "/ts" || input == "/timestamps") {
            show_ts = !show_ts;
            cli_print(std::string("  timestamps ") + (show_ts ? "on" : "off")); continue;
        }
        if(input.rfind("/ignore ", 0) == 0) {
            std::string user = trim(input.substr(8));
            if(!user.empty()) { ignored.insert(user); cli_print("  ignoring " + user); } continue;
        }
        if(input.rfind("/unignore ", 0) == 0) {
            ignored.erase(trim(input.substr(10)));
            cli_print("  unignored " + trim(input.substr(10))); continue;
        }
        if(input == "/ignored") {
            if(ignored.empty()) { cli_print("  ignore list empty"); }
            else { std::string out = "  ignored: "; for(auto& u : ignored) out += u + "  "; cli_print(out); }
            continue;
        }

        // ---- /adminkey ----
        if(input.rfind("/key", 0) == 0 && (input.size() == 4 || input[4] == ' ')) {
            std::string rest = trim(input.size() > 4 ? input.substr(4) : "");
            std::string cur_key, cur_role;
            { std::lock_guard<std::mutex> lk2(io_mtx); cur_key = my_key; cur_role = my_role; }

            if(rest == "show") {
                if(cur_key.empty()) cli_print("  no admin/mod key for this session");
                else cli_print("  key (" + cur_role + "): " + cur_key);
            } else if(rest == "add") {
                if(cur_key.empty()) { cli_print("  no key to save (not admin or mod)"); }
                else {
                    std::string hpk = hp_key(host, port);
                    cfg.admin_keys[hpk] = cur_key; save_cfg(cfg);
                    cli_print("  " + cur_role + " key saved for " + hpk);
                }
            } else if(rest.rfind("elevate ", 0) == 0) {
                // Send to server; it will respond with ADMIN_GRANTED or MOD_GRANTED
                net::send_line(s, proto::make_msg("/adminkey elevate " + trim(rest.substr(8))), &cipher);
            } else {
                cli_print("  usage: /key add | show | elevate <key>");
            }
            continue;
        }

        // ---- /savenick ----
        if(input.rfind("/savenick", 0) == 0) {
            std::string arg = trim(input.size() > 9 ? input.substr(9) : "");
            std::string cur_name, cur_color;
            { std::lock_guard<std::mutex> lk2(io_mtx); cur_name = my_name; cur_color = my_color; }

            // Find if cur_name already exists in saved list
            int existing_idx = -1;
            for(size_t i = 0; i < cfg.usernames.size(); ++i)
                if(cfg.usernames[i].first == cur_name) { existing_idx = (int)i; break; }

            auto colored_name = [&]() {
                return colorize_name(cur_name, cur_color) + ansi_reset();
            };
            if(arg.empty()) {
                if(existing_idx >= 0) {
                    cli_print("  " + colored_name() + " already saved.");
                    cli_print("  /savenick force  to overwrite  |  /savenick new  to add as new entry");
                } else if(saved_username_idx >= 0 && saved_username_idx < (int)cfg.usernames.size()) {
                    std::string orig = cfg.usernames[saved_username_idx].first;
                    std::string orig_col = cfg.usernames[saved_username_idx].second;
                    cli_print("  this will overwrite saved username "
                        + colorize_name(orig, orig_col) + ansi_reset() + ".");
                    cli_print("  /savenick force  to overwrite  |  /savenick new  to save as new entry");
                } else {
                    cfg.usernames.push_back({cur_name, cur_color}); save_cfg(cfg);
                    cli_print("  saved as " + colored_name());
                }
            } else if(arg == "force") {
                if(existing_idx >= 0) {
                    cfg.usernames[existing_idx] = {cur_name, cur_color};
                } else if(saved_username_idx >= 0 && saved_username_idx < (int)cfg.usernames.size()) {
                    cfg.usernames[saved_username_idx] = {cur_name, cur_color};
                } else {
                    cfg.usernames.push_back({cur_name, cur_color});
                }
                save_cfg(cfg); cli_print("  saved as " + colored_name());
            } else if(arg == "new") {
                cfg.usernames.push_back({cur_name, cur_color}); save_cfg(cfg);
                cli_print("  added new entry: " + colored_name());
            } else {
                cli_print("  usage: /savenick [force|new]");
            }
            continue;
        }

        // ---- /saveserver ----
        if(input.rfind("/saveserver", 0) == 0) {
            std::string arg = trim(input.size() > 11 ? input.substr(11) : "");
            std::string hpk = hp_key(host, port);

            int existing_idx = -1;
            for(size_t i = 0; i < cfg.servers.size(); ++i)
                if(cfg.servers[i].first == host && cfg.servers[i].second == port)
                    { existing_idx = (int)i; break; }

            if(arg.empty()) {
                if(existing_idx >= 0) {
                    cli_print("  " + hpk + " is already in your server list.");
                    cli_print("  /saveserver force  to overwrite  |  /saveserver new  to add duplicate");
                } else {
                    cfg.servers.push_back({host, port}); save_cfg(cfg);
                    cli_print("  server " + hpk + " saved");
                }
            } else if(arg == "force") {
                if(existing_idx >= 0) cfg.servers[existing_idx] = {host, port};
                else cfg.servers.push_back({host, port});
                save_cfg(cfg); cli_print("  server " + hpk + " saved");
            } else if(arg == "new") {
                cfg.servers.push_back({host, port}); save_cfg(cfg);
                cli_print("  added new entry for " + hpk);
            } else {
                cli_print("  usage: /saveserver [force|new]");
            }
            continue;
        }

        if(input == "/help") {
            // Show client-local-only commands, then forward to server for server-side help
            {
                std::lock_guard<std::mutex> lk2(io_mtx);
                std::cout << "\r\033[2K  -- client commands --\n";
                std::cout << "  /pause                    - pause incoming chat display\n";
                std::cout << "  /play  /unpause           - resume and replay buffered messages\n";
                std::cout << "  /log on|off               - toggle client-side chat logging\n";
                std::cout << "  /updateclient             - check for a newer client version\n";
                std::cout << "  /updateclient confirm     - download and apply the update\n";
                std::cout << "  /updateclient force       - force update even if already on latest\n";
                std::cout << "  /savenick [force|new]     - save current nickname to profile\n";
                std::cout << "  /saveserver [force|new]   - save current server to server list\n";
                std::cout << "  /key add                  - save current admin/mod key to this server\n";
                std::cout << "  /key show                 - display current session key\n";
                std::cout << "  /key elevate <key>        - elevate session with a key\n";
                std::cout << "  /ignore <user>            - ignore user\n";
                std::cout << "  /unignore <user>\n";
                std::cout << "  /ignored                  - show ignore list\n";
                std::cout << "  /ts                       - toggle timestamps\n";
                std::cout << "  /clear                    - clear screen (also ctrl+l)\n";
                std::cout << "  /rc                       - reconnect\n";
                std::cout << "  /dc                       - disconnect\n";
                std::cout << "  -- server commands --\n";
                redraw_input(in_state);
            }
            // Forward to server so it responds with role-appropriate server commands
            net::send_line(s, proto::make_msg("/help"), &cipher);
            continue;
        }

        // Track /color changes locally so /savenick can use the new color
        if(input.rfind("/color ", 0) == 0) {
            std::string new_col = normalize_hex_hash(trim(input.substr(7)));
            if(!new_col.empty()) { std::lock_guard<std::mutex> lk2(io_mtx); my_color = new_col; }
        }

        // Everything else (including /nick, /list, /kick, /ban, /motd, /serverinfo, etc.) → server
        net::send_line(s, proto::make_msg(sanitize_message(input)), &cipher);
    }

    if(rx.joinable()) rx.join();
    if(input_thr.joinable()) input_thr.join();
    net::clear_buffer(s);
    closesocket_cross(s);
    if(want_reconnect) { std::cout << "\nreconnecting...\n"; return true; }
    return false;
}

// ============================================================
// Menu helpers
// ============================================================

struct MenuItem { int idx; std::string label; };

static int menu_prompt(const std::string& title, const std::vector<MenuItem>& items) {
    std::cout << "\n=== " << title << " ===\n";
    for(auto& it : items) std::cout << it.idx << ") " << it.label << "\n";
    std::cout << "> ";
    std::string line; std::getline(std::cin, line);
    try { return std::stoi(trim(line)); } catch(...) { return -1; }
}

// Builds the display label for a saved server entry (with ping info and admin badge).
static std::string server_label(const std::string& host, uint16_t port,
                                const PingResult& pr,
                                const ClientConfig& cfg) {
    std::string hpk = hp_key(host, port);
    bool has_key = cfg.admin_keys.count(hpk) > 0;
    std::string badge = has_key ? "\033[32ma\033[0m " : "  ";

    auto dit = cfg.server_display_names.find(hpk);
    std::string custom_name = (dit != cfg.server_display_names.end()) ? dit->second : "";

    // Colorize the polled server name if a color was broadcast
    auto colorize_server_name = [](const std::string& name, const std::string& color) -> std::string {
        if(color.empty()) return name;
        return colorize_name(name, color) + ansi_reset();
    };

    std::string ip_part = cfg.show_server_ip ? (host + ":" + std::to_string(port)) : "";

    std::string label = badge;
    if(pr.ok) {
        std::string polled_name = colorize_server_name(pr.name, pr.name_color);
        std::string polled = polled_name
                           + " (" + std::to_string(pr.online) + "/" + std::to_string(pr.max_conn) + ")"
                           + " " + std::to_string(pr.latency_ms) + "ms";
        if(!custom_name.empty()) {
            label += custom_name + " - " + polled;
            if(!ip_part.empty()) label += " [" + ip_part + "]";
        } else {
            if(!ip_part.empty()) label += ip_part + "  ";
            label += polled;
        }
        if(pr.pass_req) label += " [pass]";
    } else {
        if(!custom_name.empty()) {
            label += custom_name;
            if(!ip_part.empty()) label += " [" + ip_part + "]";
            label += " (unreachable)";
        } else {
            label += (!ip_part.empty() ? ip_part : (host + ":" + std::to_string(port))) + " (unreachable)";
        }
    }
    return label;
}

// ============================================================
// main
// ============================================================

int main(int argc, char** argv) {
#ifndef _WIN32
    if(ensure_terminal_attached(argc, argv)) return 0;
#else
    enable_windows_ansi();
#endif

    ClientConfig cfg;
    {
        std::ifstream f("opicochat.cfg");
        if(f.good()) {
            Ini ini;
            std::string ln;
            while(std::getline(f, ln)) {
                ln = trim(ln);
                if(ln.empty() || ln[0]=='#' || ln[0]==';' || ln[0]=='[') continue;
                auto pos = ln.find('='); if(pos == std::string::npos) continue;
                ini.set(trim(ln.substr(0, pos)), trim(ln.substr(pos+1)));
            }
            cfg = ClientConfig::from_ini(ini);
        }
    }

    std::set<std::string> ignored;

    while(true) {
        int c = menu_prompt(std::string("opicochat v") + APP_VERSION, {
            {1, "connect"},
            {2, "settings"},
            {3, "check for updates"},
            {0, "quit"},
        });
        if(c == 0) break;

        // ===== CONNECT =====
        if(c == 1) {
            // Build connect-screen server list
            // Option 1 = manual entry, 2+ = saved servers
            // "last connected" shown as its own entry if not already in saved list
            std::string host; uint16_t port = 24816;
            // Determine if last_host is in saved list
            int last_in_saved = -1;
            if(!cfg.last_host.empty()) {
                for(size_t i = 0; i < cfg.servers.size(); ++i)
                    if(cfg.servers[i].first == cfg.last_host && cfg.servers[i].second == cfg.last_port)
                        { last_in_saved = (int)i; break; }
            }

            bool has_servers = !cfg.servers.empty() || (!cfg.last_host.empty() && last_in_saved < 0);

            bool manual_server = false;  // true = user typed address manually, no saved-key lookup

            if(!has_servers) {
                // No saved servers at all — just ask for address
                std::cout << "server host[:port]: ";
                std::string hp; std::getline(std::cin, hp);
                if(!parse_host_port(trim(hp), host, port, 24816)) { std::cout << "invalid.\n"; continue; }
                manual_server = true;
            } else {
                // Ping saved servers
                std::cout << "\npinging servers...\n";
                std::vector<PingResult> pings(cfg.servers.size());
                for(size_t i = 0; i < cfg.servers.size(); ++i) {
                    std::cout << "  " << cfg.servers[i].first << ":" << cfg.servers[i].second << "...\r" << std::flush;
                    pings[i] = ping_server(cfg.servers[i].first, cfg.servers[i].second);
                }

                // Also ping last_host if not in saved list
                PingResult last_ping;
                if(!cfg.last_host.empty() && last_in_saved < 0) {
                    last_ping = ping_server(cfg.last_host, cfg.last_port);
                }

                std::vector<MenuItem> srv_items;
                srv_items.push_back({0, "back"});
                srv_items.push_back({1, "enter address manually"});

                int next_idx = 2;
                int last_menu_idx = -1;

                // Show "last connected" if not in saved list
                if(!cfg.last_host.empty() && last_in_saved < 0) {
                    std::string lbl = "last: " + server_label(cfg.last_host, cfg.last_port, last_ping, cfg);
                    srv_items.push_back({next_idx, lbl});
                    last_menu_idx = next_idx;
                    ++next_idx;
                }

                // Saved servers
                for(size_t i = 0; i < cfg.servers.size(); ++i) {
                    srv_items.push_back({next_idx, server_label(cfg.servers[i].first, cfg.servers[i].second, pings[i], cfg)});
                    ++next_idx;
                }

                int pick = menu_prompt("connect", srv_items);
                if(pick == 0) continue;
                if(pick == 1) {
                    std::cout << "server host[:port]: ";
                    std::string hp; std::getline(std::cin, hp);
                    if(!parse_host_port(trim(hp), host, port, 24816)) { std::cout << "invalid.\n"; continue; }
                    manual_server = true;
                } else if(last_menu_idx >= 0 && pick == last_menu_idx) {
                    host = cfg.last_host; port = cfg.last_port;
                } else {
                    // Map pick to saved server index
                    int base = (last_menu_idx >= 0) ? last_menu_idx + 1 : 2;
                    int idx = pick - base;
                    if(idx < 0 || (size_t)idx >= cfg.servers.size()) continue;
                    host = cfg.servers[idx].first; port = cfg.servers[idx].second;
                }
            }

            // Choose username
            // Option 1 = manual entry, 2+ = saved usernames
            std::string user, hex;
            int saved_username_idx = -1;

            if(cfg.usernames.empty()) {
                std::cout << "username: "; std::getline(std::cin, user); user = trim(user);
                std::cout << "color (#RRGGBB or blank): "; std::string hx; std::getline(std::cin, hx);
                hex = normalize_hex_hash(trim(hx));
            } else {
                std::vector<MenuItem> usr_items;
                usr_items.push_back({0, "back"});
                usr_items.push_back({1, "enter username manually"});
                for(size_t i = 0; i < cfg.usernames.size(); ++i) {
                    std::string label = colorize_name(cfg.usernames[i].first, cfg.usernames[i].second) + ansi_reset();
                    if(!cfg.usernames[i].second.empty())
                        label += "  " + ansi_dim() + cfg.usernames[i].second + ansi_reset();
                    usr_items.push_back({(int)i+2, label});
                }
                int pick = menu_prompt("username", usr_items);
                if(pick == 0) continue;
                if(pick == 1) {
                    std::cout << "username: "; std::getline(std::cin, user); user = trim(user);
                    std::cout << "color (#RRGGBB or blank): "; std::string hx; std::getline(std::cin, hx);
                    hex = normalize_hex_hash(trim(hx));
                } else {
                    int idx = pick - 2;
                    if(idx < 0 || (size_t)idx >= cfg.usernames.size()) continue;
                    user = cfg.usernames[idx].first; hex = cfg.usernames[idx].second;
                    saved_username_idx = idx;
                }
            }

            if(user.empty()) { std::cout << "username cannot be empty.\n"; continue; }

            std::string key, pw;
            // Only apply saved admin key when connecting from a saved/last entry, not manual input
            if(!manual_server) {
                auto it = cfg.admin_keys.find(hp_key(host, port));
                if(it != cfg.admin_keys.end()) key = it->second;
            }
            { auto it = cfg.server_passwords.find(hp_key(host, port)); if(it != cfg.server_passwords.end()) pw = it->second; }

            for(;;) {
                bool again = run_chat(host, port, user, hex, key, pw, saved_username_idx,
                                      cfg.show_timestamps, ignored, cfg);
                if(!again) break;
            }
            cfg.last_host = host; cfg.last_port = port; cfg.last_username = user;
            save_cfg(cfg);
            continue;
        }

        // ===== CHECK FOR UPDATES =====
        if(c == 3) {
            std::cout << "checking for updates...\n";
            std::string json = update_http_get(GITHUB_RELEASES_API);
            if(json.empty()) {
                std::cout << "failed to reach github. check curl is installed (linux/mac) or powershell is available (windows).\n";
            } else {
                std::string tag = update_get_latest_tag(json);
                std::string latest = tag;
                if(!latest.empty() && latest[0] == 'v') latest = latest.substr(1);
                if(latest.empty()) {
                    std::cout << "could not parse response from github.\n";
                } else if(latest == APP_VERSION) {
                    std::cout << "already up to date (v" << APP_VERSION << ").\n";
                } else {
                    std::string url = update_find_asset_url(json, CLIENT_UPDATE_ASSET);
                    if(url.empty()) {
                        std::cout << "update v" << latest << " found but no binary for this platform.\n";
                    } else {
                        std::cout << "update available: v" << latest << " (current: v" << APP_VERSION << ")\n";
                        std::cout << "apply now? (1=yes, 0=no): ";
                        std::string ans; std::getline(std::cin, ans);
                        if(trim(ans) == "1") {
                            std::string exe = get_self_exe_path();
                            if(exe.empty()) { std::cout << "could not determine executable path.\n"; continue; }
#ifdef _WIN32
                            std::string bat = update_write_bat(exe, false);
                            if(bat.empty()) { std::cout << "failed to write updater batch file.\n"; continue; }
                            update_launch_file(bat);
                            std::cout << "updater launched. close the client to apply the update.\n";
                            std::exit(0);
#else
                            std::string tmp = exe + ".update";
                            std::cout << "downloading v" << latest << "...\n";
                            if(!update_download_file(url, tmp)) {
                                std::cout << "download failed.\n";
                            } else {
                                std::string msg;
                                if(update_apply_binary(tmp, exe, msg)) {
                                    std::cout << msg << " relaunching...\n";
                                    execl(exe.c_str(), exe.c_str(), (char*)nullptr);
                                    std::cout << "execl failed — restart manually.\n";
                                } else {
                                    std::cout << "update failed: " << msg << "\n";
                                }
                            }
#endif
                        }
                    }
                }
            }
            continue;
        }

        // ===== SETTINGS =====
        if(c == 2) {
            for(;;) {
                int sc = menu_prompt("settings", {
                    {0, "back"},
                    {1, "manage usernames"},
                    {2, "manage servers"},
                    {3, "toggle timestamps (currently " + std::string(cfg.show_timestamps ? "on" : "off") + ")"},
                    {4, "toggle show server IP in list (currently " + std::string(cfg.show_server_ip ? "on" : "off") + ")"},
                    {5, "toggle client logging (currently " + std::string(cfg.client_log_enabled ? "on" : "off") + ")"},
                });
                if(sc == 0) break;

                // --- manage usernames ---
                if(sc == 1) {
                    for(;;) {
                        std::vector<MenuItem> items;
                        items.push_back({0, "back"});
                        items.push_back({1, "add new"});
                        for(size_t i = 0; i < cfg.usernames.size(); ++i) {
                            std::string label = colorize_name(cfg.usernames[i].first, cfg.usernames[i].second) + ansi_reset();
                            if(!cfg.usernames[i].second.empty())
                                label += "  " + ansi_dim() + cfg.usernames[i].second + ansi_reset();
                            items.push_back({(int)i+2, label});
                        }
                        int pick = menu_prompt("usernames", items);
                        if(pick == 0) break;
                        if(pick == 1) {
                            std::cout << "username: "; std::string name; std::getline(std::cin, name); name = trim(name);
                            if(!is_valid_username(name)) { std::cout << "invalid username.\n"; continue; }
                            std::cout << "color (#RRGGBB or blank): "; std::string hx; std::getline(std::cin, hx);
                            std::string norm = normalize_hex_hash(trim(hx));
                            if(!hx.empty() && !trim(hx).empty() && norm.empty()) std::cout << "invalid color, ignoring.\n";
                            cfg.usernames.push_back({name, norm}); save_cfg(cfg);
                        } else {
                            int idx = pick - 2;
                            if(idx < 0 || (size_t)idx >= cfg.usernames.size()) continue;
                            int act = menu_prompt(cfg.usernames[idx].first, {
                                {0, "back"}, {1, "remove"}, {2, "change color"},
                            });
                            if(act == 1) { cfg.usernames.erase(cfg.usernames.begin() + idx); save_cfg(cfg); std::cout << "removed.\n"; }
                            else if(act == 2) {
                                std::cout << "new color (#RRGGBB or blank to clear): ";
                                std::string hx; std::getline(std::cin, hx); hx = trim(hx);
                                if(hx.empty()) { cfg.usernames[idx].second = ""; save_cfg(cfg); }
                                else {
                                    std::string norm = normalize_hex_hash(hx);
                                    if(norm.empty()) std::cout << "invalid color.\n";
                                    else { cfg.usernames[idx].second = norm; save_cfg(cfg); std::cout << "color updated.\n"; }
                                }
                            }
                        }
                    }
                }

                // --- manage servers ---
                else if(sc == 2) {
                    for(;;) {
                        std::vector<MenuItem> items;
                        items.push_back({0, "back"});
                        items.push_back({1, "add new"});
                        for(size_t i = 0; i < cfg.servers.size(); ++i) {
                            auto& sv = cfg.servers[i]; std::string hpk = hp_key(sv.first, sv.second);
                            bool has_key  = cfg.admin_keys.count(hpk) > 0;
                            bool has_pass = cfg.server_passwords.count(hpk) > 0;
                            auto dit = cfg.server_display_names.find(hpk);
                            std::string label = sv.first + ":" + std::to_string(sv.second);
                            if(dit != cfg.server_display_names.end()) label += " (" + dit->second + ")";
                            if(has_key)  label += " \033[32m[admin/mod key]\033[0m";
                            if(has_pass) label += " [pass saved]";
                            items.push_back({(int)i+2, label});
                        }
                        int pick = menu_prompt("servers", items);
                        if(pick == 0) break;
                        if(pick == 1) {
                            std::cout << "host[:port]: "; std::string hp; std::getline(std::cin, hp);
                            std::string h; uint16_t p = 24816;
                            if(!parse_host_port(trim(hp), h, p, 24816)) { std::cout << "invalid.\n"; continue; }
                            std::cout << "custom display name (blank to skip): "; std::string dname; std::getline(std::cin, dname); dname = trim(dname);
                            std::cout << "admin key (blank if none): "; std::string key; std::getline(std::cin, key);
                            std::cout << "password (blank if none): "; std::string pw; std::getline(std::cin, pw);
                            cfg.servers.push_back({h, p});
                            std::string hpk = hp_key(h, p);
                            if(!dname.empty()) cfg.server_display_names[hpk] = dname;
                            if(!trim(key).empty()) cfg.admin_keys[hpk] = trim(key);
                            if(!trim(pw).empty())  cfg.server_passwords[hpk] = trim(pw);
                            save_cfg(cfg);
                        } else {
                            int idx = pick - 2;
                            if(idx < 0 || (size_t)idx >= cfg.servers.size()) continue;
                            auto& sv = cfg.servers[idx];
                            std::string hpk = hp_key(sv.first, sv.second);
                            bool has_key  = cfg.admin_keys.count(hpk) > 0;
                            bool has_pass = cfg.server_passwords.count(hpk) > 0;
                            bool has_dname = cfg.server_display_names.count(hpk) > 0;
                            int act = menu_prompt(sv.first + ":" + std::to_string(sv.second), {
                                {0, "back"},
                                {1, "remove"},
                                {2, has_dname ? "change display name" : "set display name"},
                                {3, "clear display name"},
                                {4, has_key  ? "change admin/mod key" : "set admin/mod key"},
                                {5, "clear admin key"},
                                {6, has_pass ? "change saved password" : "save password"},
                                {7, "clear saved password"},
                            });
                            if(act == 1) {
                                cfg.admin_keys.erase(hpk); cfg.server_passwords.erase(hpk);
                                cfg.server_display_names.erase(hpk);
                                cfg.servers.erase(cfg.servers.begin() + idx); save_cfg(cfg); std::cout << "removed.\n";
                            } else if(act == 2) {
                                std::cout << "display name: "; std::string dn; std::getline(std::cin, dn); dn = trim(dn);
                                if(!dn.empty()) { cfg.server_display_names[hpk] = dn; save_cfg(cfg); std::cout << "saved.\n"; }
                            } else if(act == 3) {
                                cfg.server_display_names.erase(hpk); save_cfg(cfg); std::cout << "cleared.\n";
                            } else if(act == 4) {
                                std::cout << "admin key: "; std::string key; std::getline(std::cin, key); key = trim(key);
                                if(!key.empty()) { cfg.admin_keys[hpk] = key; save_cfg(cfg); std::cout << "saved.\n"; }
                            } else if(act == 5) {
                                cfg.admin_keys.erase(hpk); save_cfg(cfg); std::cout << "key cleared.\n";
                            } else if(act == 6) {
                                std::cout << "password: "; std::string pw; std::getline(std::cin, pw); pw = trim(pw);
                                if(!pw.empty()) { cfg.server_passwords[hpk] = pw; save_cfg(cfg); std::cout << "saved.\n"; }
                            } else if(act == 7) {
                                cfg.server_passwords.erase(hpk); save_cfg(cfg); std::cout << "password cleared.\n";
                            }
                        }
                    }
                }

                else if(sc == 3) {
                    cfg.show_timestamps = !cfg.show_timestamps; save_cfg(cfg);
                    std::cout << "timestamps " << (cfg.show_timestamps ? "on" : "off") << "\n";
                }
                else if(sc == 4) {
                    cfg.show_server_ip = !cfg.show_server_ip; save_cfg(cfg);
                    std::cout << "show server IP " << (cfg.show_server_ip ? "on" : "off") << "\n";
                }
                else if(sc == 5) {
                    cfg.client_log_enabled = !cfg.client_log_enabled; save_cfg(cfg);
                    std::cout << "client logging " << (cfg.client_log_enabled ? "on" : "off") << "\n";
                }
            }
        }
    }

    return 0;
}
