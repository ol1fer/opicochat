#include "utils.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include "common.hpp" // for format_hhmm

#ifndef _WIN32
  #include <unistd.h>
  #include <sys/stat.h>
  #ifdef __APPLE__
    #include <mach-o/dyld.h>
  #endif
#else
  #include <windows.h>
#endif

std::string trim(const std::string& s) {
    size_t a = 0;
    while(a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while(b > a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}

bool iequals(const std::string& a, const std::string& b) {
    if(a.size() != b.size()) return false;
    for(size_t i = 0; i < a.size(); ++i)
        if(std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

std::string sanitize_message(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for(char c : s) {
        if(c == '\n' || c == '\r') continue;
        out.push_back(c);
    }
    return out;
}

bool parse_host_port(const std::string& input, std::string& host, uint16_t& port, uint16_t def_port) {
    // handle IPv6 literal [::1]:port
    if(!input.empty() && input[0] == '[') {
        auto close = input.find(']');
        if(close == std::string::npos) return false;
        host = input.substr(1, close - 1);
        if(close + 1 < input.size() && input[close+1] == ':') {
            try { port = (uint16_t)std::stoi(input.substr(close + 2)); }
            catch(...) { port = def_port; }
        } else {
            port = def_port;
        }
        return !host.empty();
    }
    auto pos = input.rfind(':');
    if(pos == std::string::npos) {
        host = input; port = def_port;
    } else {
        host = input.substr(0, pos);
        try { port = (uint16_t)std::stoi(input.substr(pos + 1)); }
        catch(...) { port = def_port; }
    }
    return !host.empty();
}

// ---------- ANSI ----------

static int hex2int(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if(c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return 0;
}

std::string ansi_for_hex(const std::string& hex) {
    if(hex.size() != 7 || hex[0] != '#') return "";
    int r = hex2int(hex[1])*16 + hex2int(hex[2]);
    int g = hex2int(hex[3])*16 + hex2int(hex[4]);
    int b = hex2int(hex[5])*16 + hex2int(hex[6]);
    return "\x1b[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

std::string ansi_reset()  { return "\x1b[0m"; }
std::string ansi_dim()    { return "\x1b[2m"; }
std::string ansi_italic() { return "\x1b[3m"; }
std::string ansi_bold()   { return "\x1b[1m"; }

#ifdef _WIN32
void enable_windows_ansi() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if(hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if(!GetConsoleMode(hOut, &mode)) return;
    SetConsoleMode(hOut, mode | 0x0004 | 0x0008); // VIRTUAL_TERMINAL + DISABLE_NEWLINE_AUTO_RETURN
}
#endif

// ---------- Username validation ----------

bool is_valid_username(const std::string& name) {
    if(name.size() < 2 || name.size() > 20) return false;
    if(name[0] == '_') return false;
    for(char c : name)
        if(!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

bool is_reserved_name(const std::string& name) {
    static const char* reserved[] = {
        "server", "[server]", "system", "admin", "console", nullptr
    };
    for(int i = 0; reserved[i]; ++i)
        if(iequals(name, reserved[i])) return true;
    return false;
}

// ---------- Hex color ----------

bool is_valid_hex_color(const std::string& in) {
    std::string s = in;
    if(s.size() == 7 && s[0] == '#') s = s.substr(1);
    if(s.size() != 6) return false;
    for(char c : s) if(!std::isxdigit((unsigned char)c)) return false;
    return true;
}

std::string normalize_hex_hash(const std::string& in) {
    if(!is_valid_hex_color(in)) return "";
    std::string s = (in.size() == 7 && in[0] == '#') ? in.substr(1) : in;
    for(char& c : s) c = (char)std::toupper((unsigned char)c);
    return "#" + s;
}

// ---------- Color rendering ----------

bool term_supports_truecolor() {
#ifdef _WIN32
    return true;
#else
    const char* c = std::getenv("COLORTERM");
    if(c && (strstr(c,"truecolor") || strstr(c,"24bit"))) return true;
    const char* t = std::getenv("TERM");
    if(t && (strstr(t,"kitty")||strstr(t,"tmux")||strstr(t,"screen")||strstr(t,"xterm")||strstr(t,"256color"))) return true;
    return false;
#endif
}

static void hsl_to_rgb(float h, float s, float l, int& R, int& G, int& B) {
    auto hue2rgb = [](float p, float q, float tt) {
        if(tt < 0) tt += 1;
        if(tt > 1) tt -= 1;
        if(tt < 1.f/6) return p + (q-p)*6*tt;
        if(tt < 0.5f)  return q;
        if(tt < 2.f/3) return p + (q-p)*(2.f/3 - tt)*6;
        return p;
    };
    float q = l < 0.5f ? l*(1+s) : l+s-l*s;
    float p = 2*l - q;
    R = (int)(hue2rgb(p,q,h+1.f/3)*255 + 0.5f);
    G = (int)(hue2rgb(p,q,h      )*255 + 0.5f);
    B = (int)(hue2rgb(p,q,h-1.f/3)*255 + 0.5f);
}

static uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for(unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

static std::string truecolor(int R, int G, int B) {
    return "\x1b[38;2;" + std::to_string(R) + ";" + std::to_string(G) + ";" + std::to_string(B) + "m";
}

std::string colorize_name(const std::string& name, const std::string& preferred_hex) {
    // [server] and other bracket-wrapped names in pink
    if(!name.empty() && name.front() == '[' && name.back() == ']') {
        std::string inner = name.substr(1, name.size()-2);
        std::string pink = truecolor(255, 100, 180);
        return "\x1b[0m[" + pink + inner + ansi_reset() + "]";
    }

    std::string code;
    std::string hex = normalize_hex_hash(preferred_hex);
    if(!hex.empty()) {
        code = ansi_for_hex(hex);
    }
    if(code.empty()) {
        uint32_t h = fnv1a(name);
        float hue = (h % 360) / 360.0f;
        int R, G, B; hsl_to_rgb(hue, 0.65f, 0.58f, R, G, B);
        if(term_supports_truecolor()) {
            code = truecolor(R, G, B);
        } else {
            int r = R*6/256, g = G*6/256, b = B*6/256;
            code = "\x1b[38;5;" + std::to_string(16 + 36*r + 6*g + b) + "m";
        }
    }
    return "\x1b[0m<" + code + name + ansi_reset() + ">";
}

std::string format_ts_prefix(const std::string& iso) {
    std::string hhmm = format_hhmm(iso);
    if(hhmm.empty()) return "";
    return ansi_dim() + "[" + hhmm + "]" + ansi_reset() + " ";
}

// ---------- Terminal spawn ----------

static std::string get_exe_path(int argc, char** argv) {
#ifdef __APPLE__
    uint32_t size = 0; _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1);
    if(_NSGetExecutablePath(buf.data(), &size) == 0) { buf[size] = 0; return buf.data(); }
    return argc > 0 ? argv[0] : "";
#elif defined(_WIN32)
    char buf[MAX_PATH]; DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf, n ? n : 0);
#else
    char buf[4096]; ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    if(n > 0) { buf[n] = 0; return buf; }
    return argc > 0 ? argv[0] : "";
#endif
}

// ---------- Self-update helpers ----------

std::string get_self_exe_path() {
#ifdef __APPLE__
    uint32_t size = 0; _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1);
    if(_NSGetExecutablePath(buf.data(), &size) == 0) { buf[size] = '\0'; return buf.data(); }
    return "";
#elif defined(_WIN32)
    char buf[MAX_PATH]; DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return n ? std::string(buf, n) : "";
#else
    char buf[4096]; ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    if(n > 0) { buf[n] = '\0'; return buf; }
    return "";
#endif
}

std::string update_http_get(const std::string& url) {
#ifdef _WIN32
    std::string cmd = "powershell -NoProfile -Command \""
        "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; "
        "(Invoke-WebRequest -Uri '" + url + "' -UseBasicParsing "
        "-Headers @{'User-Agent'='opicochat-updater'}).Content\" 2>nul";
    FILE* p = _popen(cmd.c_str(), "r");
#else
    std::string cmd = "curl -fsSL --max-time 15 "
        "-H \"User-Agent: opicochat-updater\" \"" + url + "\" 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if(!p) return "";
    std::string result; char buf[4096];
    while(fgets(buf, sizeof(buf), p)) result += buf;
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    return result;
}

// Robust JSON string-value extractor. Finds "key" anywhere in json (from start),
// skips optional whitespace and ':', then returns the quoted string value.
static std::string json_str_value(const std::string& json, const std::string& key, size_t start = 0) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle, start);
    if(pos == std::string::npos) return "";
    pos += needle.size();
    while(pos < json.size() && (json[pos]==' '||json[pos]=='\t'||json[pos]=='\n'||json[pos]=='\r')) ++pos;
    if(pos >= json.size() || json[pos] != ':') return "";
    ++pos;
    while(pos < json.size() && (json[pos]==' '||json[pos]=='\t'||json[pos]=='\n'||json[pos]=='\r')) ++pos;
    if(pos >= json.size() || json[pos] != '"') return "";
    ++pos;
    auto end = json.find('"', pos);
    if(end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

std::string update_get_latest_tag(const std::string& json) {
    return json_str_value(json, "tag_name");
}

std::string update_find_asset_url(const std::string& json, const std::string& asset_name) {
    // Walk all "name" fields until we find asset_name, then grab browser_download_url after it.
    const std::string name_key = "\"name\"";
    size_t search = 0;
    for(;;) {
        auto kpos = json.find(name_key, search);
        if(kpos == std::string::npos) return "";
        // Read the value of this "name" field
        size_t p = kpos + name_key.size();
        while(p < json.size() && (json[p]==' '||json[p]=='\t'||json[p]=='\n'||json[p]=='\r')) ++p;
        if(p >= json.size() || json[p] != ':') { search = kpos + 1; continue; }
        ++p;
        while(p < json.size() && (json[p]==' '||json[p]=='\t'||json[p]=='\n'||json[p]=='\r')) ++p;
        if(p >= json.size() || json[p] != '"') { search = kpos + 1; continue; }
        ++p;
        auto end = json.find('"', p);
        if(end == std::string::npos) return "";
        if(json.substr(p, end - p) != asset_name) { search = kpos + 1; continue; }
        // Found the right asset — look for browser_download_url from this position
        return json_str_value(json, "browser_download_url", kpos);
    }
}

bool update_download_file(const std::string& url, const std::string& dest) {
#ifdef _WIN32
    std::string cmd = "powershell -NoProfile -Command \""
        "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; "
        "Invoke-WebRequest -Uri '" + url + "' -OutFile '" + dest +
        "' -UseBasicParsing\" 2>nul";
    return system(cmd.c_str()) == 0;
#else
    std::string cmd = "curl -fsSL --max-time 120 \"" + url + "\" -o \"" + dest + "\" 2>/dev/null";
    return system(cmd.c_str()) == 0;
#endif
}

bool update_apply_binary(const std::string& downloaded, const std::string& exe_path, std::string& msg) {
#ifdef _WIN32
    (void)downloaded; (void)exe_path;
    msg = "use the updater batch file on Windows.";
    return false;
#else
    chmod(downloaded.c_str(), 0755);
    if(rename(downloaded.c_str(), exe_path.c_str()) != 0) {
        msg = "failed to replace binary — check permissions.";
        return false;
    }
    msg = "update applied.";
    return true;
#endif
}

std::string update_write_bat(const std::string& exe_path, bool is_server) {
#ifdef _WIN32
    std::string dir;
    auto sep = exe_path.rfind('\\');
    if(sep != std::string::npos) dir = exe_path.substr(0, sep + 1);
    std::string bat_name = is_server ? "opicochatserverupdater.bat" : "opicochatclientupdater.bat";
    std::string bat_path = dir + bat_name;
    std::string url = is_server
        ? "https://github.com/ol1fer/opicochat/releases/latest/download/opicochat_server-windows-x64.exe"
        : "https://github.com/ol1fer/opicochat/releases/latest/download/opicochat-windows-x64.exe";
    std::string display = is_server ? "opicochat_server" : "opicochat";
    std::ofstream f(bat_path);
    if(!f) return "";
    f << "@echo off\r\n";
    f << "setlocal EnableDelayedExpansion\r\n";
    f << "set \"EXE=" << exe_path << "\"\r\n";
    f << "set \"OPICO_DL=%EXE%_update.exe\"\r\n";
    f << "set \"URL=" << url << "\"\r\n";
    f << "\r\n";
    f << "echo Downloading latest " << display << "...\r\n";
    f << "powershell -NoProfile -Command \"[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri '%URL%' -OutFile '%OPICO_DL%' -UseBasicParsing\" 2>nul\r\n";
    f << "\r\n";
    f << "if not exist \"%OPICO_DL%\" (\r\n";
    f << "    echo Download failed. Check your internet connection.\r\n";
    f << "    pause\r\n";
    f << "    exit /b 1\r\n";
    f << ")\r\n";
    f << "\r\n";
    f << "echo Waiting for " << display << " to close...\r\n";
    f << ":waitloop\r\n";
    f << "move /y \"%OPICO_DL%\" \"%EXE%\" >nul 2>&1\r\n";
    f << "if errorlevel 1 (\r\n";
    f << "    timeout /t 1 /nobreak >nul\r\n";
    f << "    goto waitloop\r\n";
    f << ")\r\n";
    f << "\r\n";
    f << "echo.\r\n";
    f << "echo " << display << " has been updated successfully.\r\n";
    if(is_server) {
        f << "set /p \"RELAUNCH=Relaunch server now? [Y/n]: \"\r\n";
        f << "if \"!RELAUNCH!\"==\"\" set \"RELAUNCH=y\"\r\n";
        f << "if /i \"!RELAUNCH!\"==\"y\" (\r\n";
        f << "    start \"\" \"%EXE%\"\r\n";
        f << ")\r\n";
    } else {
        f << "set /p \"RELAUNCH=Relaunch client now? [Y/n]: \"\r\n";
        f << "if \"!RELAUNCH!\"==\"\" set \"RELAUNCH=y\"\r\n";
        f << "if /i \"!RELAUNCH!\"==\"y\" (\r\n";
        f << "    start \"\" \"%EXE%\"\r\n";
        f << ")\r\n";
    }
    f << "(goto) 2>nul & del \"%~f0\" & exit\r\n";
    return bat_path;
#else
    (void)exe_path; (void)is_server;
    return "";
#endif
}

void update_launch_file(const std::string& path) {
#ifdef _WIN32
    std::string cmd = "start \"\" \"" + path + "\"";
    system(cmd.c_str());
#else
    (void)path;
#endif
}

bool ensure_terminal_attached(int argc, char** argv) {
#ifdef _WIN32
    return false;
#else
    if(isatty(STDIN_FILENO) || isatty(STDOUT_FILENO)) return false;
    std::string exe = get_exe_path(argc, argv);
    std::string args;
    for(int i = 1; i < argc; ++i) { args += " \""; args += argv[i]; args += "\""; }
#ifdef __APPLE__
    std::string cmd = "osascript -e 'tell application \"Terminal\" to do script \"" + exe + args + "\"' >/dev/null 2>&1 &";
    std::system(cmd.c_str());
    return true;
#else
    const char* terms[] = {
        "x-terminal-emulator -e", "kitty -e", "alacritty -e",
        "wezterm start --", "gnome-terminal --", "konsole -e",
        "xfce4-terminal -e", "xterm -e", nullptr
    };
    for(int i = 0; terms[i]; ++i) {
        std::string cmd = std::string(terms[i]) + " \"" + exe + "\"" + args + " >/dev/null 2>&1 &";
        if(std::system(cmd.c_str()) == 0) return true;
    }
    return true;
#endif
#endif
}
