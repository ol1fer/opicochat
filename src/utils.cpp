#include "utils.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>

#ifndef _WIN32
  #include <unistd.h>
  #ifdef __APPLE__
    #include <mach-o/dyld.h>
  #endif
#else
  #include <windows.h>
#endif

std::string trim(const std::string& s){ size_t a=0; while(a<s.size() && std::isspace((unsigned char)s[a])) a++; size_t b=s.size(); while(b>a && std::isspace((unsigned char)s[b-1])) b--; return s.substr(a,b-a); }

bool iequals(const std::string& a, const std::string& b){ if(a.size()!=b.size()) return false; for(size_t i=0;i<a.size();++i){ if(std::tolower((unsigned char)a[i])!=std::tolower((unsigned char)b[i])) return false; } return true; }

bool contains_brackets(const std::string& s){ return s.find('[')!=std::string::npos || s.find(']')!=std::string::npos; }

std::string sanitize_message(const std::string& s){ std::string out; out.reserve(s.size()); for(char c: s){ if(c=='\n' || c=='\r') continue; out.push_back(c);} return out; }

bool parse_host_port(const std::string& input, std::string& host, uint16_t& port, uint16_t def_port){
    auto pos = input.rfind(':');
    if (pos==std::string::npos){ host = input; port = def_port; return !host.empty(); }
    host = input.substr(0,pos); try{ port = (uint16_t)std::stoi(input.substr(pos+1)); }catch(...){ port = def_port; }
    if(host.empty()) return false; return true;
}

static int hex2int(char c){ if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return 10+(c-'a'); if(c>='A'&&c<='F') return 10+(c-'A'); return 0; }

std::string ansi_for_hex(const std::string& hex){
    if(hex.size()!=7 || hex[0]!='#') return "";
    int r = hex2int(hex[1])*16 + hex2int(hex[2]);
    int g = hex2int(hex[3])*16 + hex2int(hex[4]);
    int b = hex2int(hex[5])*16 + hex2int(hex[6]);
    return "\x1b[38;2;"+std::to_string(r)+";"+std::to_string(g)+";"+std::to_string(b)+"m";
}

std::string ansi_reset(){ return "\x1b[0m"; }

#ifdef _WIN32
void enable_windows_ansi(){
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if(hOut==INVALID_HANDLE_VALUE) return;
    DWORD mode=0; if(!GetConsoleMode(hOut, &mode)) return; mode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    SetConsoleMode(hOut, mode);
}
#endif

bool is_valid_username(const std::string& name){
    if(name.empty()) return false;
    for(char c: name){
        if(!(std::isalnum((unsigned char)c) || c=='_')) return false;
    }
    return true;
}

// ---- HEX validate/normalize ----

static bool is_hex_digit(char c){ return std::isxdigit((unsigned char)c); }

bool is_valid_hex_color(const std::string& in){
    std::string s = in;
    if(s.size()==7 && s[0]=='#') s = s.substr(1);
    if(s.size()!=6) return false;
    for(char c: s) if(!is_hex_digit(c)) return false;
    return true;
}

std::string normalize_hex_hash(const std::string& in){
    if(!is_valid_hex_color(in)) return "";
    std::string s = (in.size()==7 && in[0]=='#') ? in.substr(1) : in;
    for(char& c: s) c = std::toupper((unsigned char)c);
    return "#" + s;
}

// ---- terminal colors & name rendering ----

bool term_supports_truecolor(){
#ifdef _WIN32
    return true; // Windows 10+ with ANSI enabled
#else
    const char* c = std::getenv("COLORTERM");
    if(c && (strstr(c,"truecolor") || strstr(c,"24bit"))) return true;
    const char* t = std::getenv("TERM");
    if(t && (strstr(t,"kitty") || strstr(t,"tmux") || strstr(t,"screen") || strstr(t,"xterm"))) return true;
    return false;
#endif
}

// HSL->RGB helper (0..1 inputs, outputs 0..255)
static void hsl_to_rgb(float h, float s, float l, int& R, int& G, int& B){
    auto hue2rgb = [&](float p,float q,float tt){
        if(tt<0) tt+=1; if(tt>1) tt-=1;
        if(tt<1.0f/6) return p+(q-p)*6*tt;
        if(tt<1.0f/2) return q;
        if(tt<2.0f/3) return p+(q-p)*(2.0f/3-tt)*6;
        return p;
    };
    float q = l < 0.5f ? l*(1+s) : l+s-l*s;
    float p = 2*l - q;
    float r = hue2rgb(p,q,h+1.0f/3);
    float g = hue2rgb(p,q,h);
    float b = hue2rgb(p,q,h-1.0f/3);
    R = int(r*255+0.5f); G = int(g*255+0.5f); B = int(b*255+0.5f);
}

static uint32_t fnv1a(const std::string& s){
    uint32_t h=2166136261u; for(unsigned char c: s){ h ^= c; h *= 16777619u; } return h;
}

static std::string ansi_truecolor_rgb(int R,int G,int B){
    return "\x1b[38;2;" + std::to_string(R) + ";" + std::to_string(G) + ";" + std::to_string(B) + "m";
}

std::string colorize_name(const std::string& name, const std::string& preferred_hex){
    // Brackets stay white; only the inside is colored.
    // Server tag: [server] in pink.
    if(!name.empty() && name.front()=='[' && name.back()==']'){
        std::string inner = name.substr(1, name.size()-2);
        // nice pink
        int R=255, G=92, B=173; // #FF5CAD
        std::string pink = ansi_truecolor_rgb(R,G,B);
        return "\x1b[0m[" + pink + inner + ansi_reset() + "]";
    }

    // Usernames: <coloredname>
    std::string code;
    std::string hex = normalize_hex_hash(preferred_hex);
    if(!hex.empty()){
        code = ansi_for_hex(hex);
    }
    if(code.empty()){
        // derive deterministic color from name
        uint32_t h = fnv1a(name);
        float hue = (h % 360) / 360.0f;
        int R,G,B; hsl_to_rgb(hue, 0.65f, 0.60f, R,G,B);
        if(term_supports_truecolor()) code = ansi_truecolor_rgb(R,G,B);
        else {
            int r=R*6/256, g=G*6/256, b=B*6/256;
            int idx = 16 + 36*r + 6*g + b;
            code = "\x1b[38;5;" + std::to_string(idx) + "m";
        }
    }
    return "\x1b[0m<" + code + name + ansi_reset() + ">";
}

// --- Spawn a terminal when detached (Linux/macOS) ---

static std::string get_exe_path(int argc, char** argv){
#ifdef __APPLE__
    uint32_t size=0; _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size+1);
    if(_NSGetExecutablePath(buf.data(), &size)==0){ buf[size]=0; return std::string(buf.data()); }
    return argc>0? std::string(argv[0]) : std::string();
#elif defined(_WIN32)
    char buf[MAX_PATH]; DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf, buf + (n? n : 0));
#else
    char buf[4096]; ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    if(n>0){ buf[n]=0; return std::string(buf); }
    return argc>0? std::string(argv[0]) : std::string();
#endif
}

bool ensure_terminal_attached(int argc, char** argv){
#ifdef _WIN32
    return false; // console apps already open a window on double click
#else
    if(isatty(0) || isatty(1) || isatty(2)) return false; // already in a terminal
    std::string exe = get_exe_path(argc, argv);
    std::ostringstream argstr;
    for(int i=1;i<argc;++i){ argstr << " \"" << argv[i] << "\""; }
#ifdef __APPLE__
    // Use Terminal.app
    std::string cmd = "osascript -e 'tell application \"Terminal\" to do script \"" + exe + argstr.str() + "\"' >/dev/null 2>&1 &";
    std::system(cmd.c_str());
    return true;
#else
    // Try a few common terminals
    const char* terms[] = {
        "x-terminal-emulator -e", // Debian/Ubuntu
        "kitty -e",
        "alacritty -e",
        "wezterm start --",
        "gnome-terminal --",
        "konsole -e",
        "xfce4-terminal -e",
        "xterm -e"
    };
    for(const char* t: terms){
        std::string cmd = std::string(t) + " \"" + exe + "\"" + argstr.str() + " >/dev/null 2>&1 &";
        if(std::system(cmd.c_str())==0) return true;
    }
    // last resort
    std::string cmd = "xterm -e \"" + exe + "\"" + argstr.str() + " &";
    std::system(cmd.c_str());
    return true;
#endif
#endif
}
