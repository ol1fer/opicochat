#include <sstream>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <limits>
#include <cctype>

#include "common.hpp"
#include "config.hpp"
#include "protocol.hpp"
#include "utils.hpp"
#include <iostream>
#include <fstream>

using namespace std::chrono;

#ifdef _WIN32
  #include <conio.h>
#else
  #include <unistd.h>
  #include <termios.h>
#endif

struct MenuItem { int idx; std::string label; };
static inline void print_prompt_line(const std::string& buf){
    std::cout << "\r\033[2K> " << buf << std::flush;
}

static bool g_remember_last_username = false;

// Save config + remember flag
static void save_client_cfg(const ClientConfig& cc){
    auto ini = ClientConfig::to_ini(cc);
    std::string s = ini.serialize();
    {   // drop old remember flag if present
        std::stringstream in(s); std::string out,line;
        while(std::getline(in,line)){
            if(line.rfind("remember_last_username=", 0) == 0) continue;
            out += line; out += "\n";
        }
        s.swap(out);
    }
    s += std::string("remember_last_username=") + (g_remember_last_username? "1":"0") + "\n";
    std::ofstream f("opicochat.cfg"); f << s;
    std::cout<<"Saved opicochat.cfg\n";
}

#ifndef _WIN32
struct TermRawGuard {
    termios old{}; bool active=false;
    void enable(){
        if(active) return;
        tcgetattr(STDIN_FILENO, &old);
        termios raw = old;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME]= 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        active=true;
    }
    void disable(){
        if(!active) return;
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        active=false;
    }
    ~TermRawGuard(){ disable(); }
};
#endif

static bool run_chat(const std::string& host, uint16_t port, const std::string& username, const std::string& color_hex_pref){
#ifdef _WIN32
    enable_windows_ansi();
#endif
    std::string err; socket_t s = net::connect_tcp(host, port, err);
    if(s==INVALID_SOCKET){ std::cout<<"Connect failed: "<<err<<"\n"; return false; }

    bool disconnected=false; std::string line, server_name; bool pass_required=false;

    // 1) WELCOME
    auto deadline = steady_clock::now() + std::chrono::seconds(6);
    bool got_welcome=false;
    while(steady_clock::now() < deadline){
        if(net::recv_line(s, line, 500, disconnected)){
            if(proto::parse_welcome(line, server_name, pass_required)){ got_welcome=true; break; }
        } else if(disconnected){ break; }
    }
    if(!got_welcome){ std::cout<<"No welcome from server\n"; closesocket_cross(s); return false; }

    // 2) AUTH (send normalized color or blank)
    std::string password;
    if(pass_required){ std::cout<<"Server requires a password: "; std::getline(std::cin, password); }
    std::string norm_hex = normalize_hex_hash(color_hex_pref); // "" if invalid/empty
    net::send_line(s, proto::make_auth(username, password, norm_hex));
    if(!net::recv_line(s, line, 5000, disconnected)){ std::cout<<"No auth response\n"; closesocket_cross(s); return false; }
    if(line!="AUTH_OK"){ std::cout<<line<<"\n"; closesocket_cross(s); return false; }

    std::cout << "Connected to " << server_name << " as " << username << ". Type /dc, /rc or /list.\n";

    std::atomic<bool> running{true};
    std::mutex io_mtx;                 // protects console + input buffer redraw
    std::string input_buf;

    auto redraw = [&](){
        std::lock_guard<std::mutex> lk(io_mtx);
        print_prompt_line(input_buf);
    };

    // RX thread — prints messages, then redraws input line
    std::thread rx([&]{
        std::string iso,name,text,color; bool disc=false; std::string l;
        while(running){
            if(net::recv_line(s, l, 500, disc)){
                if(proto::parse_chat(l, iso,name,color,text)){
                    std::string token = colorize_name(name, (color=="-")? "" : color);
                    std::lock_guard<std::mutex> lk(io_mtx);
                    std::cout << "\r\033[2K" << token << " " << text << "\n";
                    print_prompt_line(input_buf);
                } else {
                    std::lock_guard<std::mutex> lk(io_mtx);
                    std::cout << "\r\033[2K" << l << "\n";
                    print_prompt_line(input_buf);
                }
            } else if(disc){
                std::lock_guard<std::mutex> lk(io_mtx);
                std::cout<<"\nDisconnected.\n";
                running=false; break;
            }
        }
    });

    // INPUT thread — raw, single-line editor
    std::mutex q_mtx; std::condition_variable q_cv; std::deque<std::string> q;

    std::thread input_thr([&]{
#ifdef _WIN32
        // no echo, line-at-a-time via _getch
        for(;;){
            if(!running) break;
            if(_kbhit()){
                int ch = _getch();
                if(ch=='\r' || ch=='\n'){
                    std::string out = input_buf;
                    input_buf.clear();
                    {
                        std::lock_guard<std::mutex> lk(io_mtx);
                        print_prompt_line(input_buf); // clear line
                    }
                    {
                        std::lock_guard<std::mutex> lk(q_mtx);
                        q.push_back(out);
                    }
                    q_cv.notify_one();
                } else if(ch==8 || ch==127){ // backspace
                    if(!input_buf.empty()) input_buf.pop_back();
                    redraw();
                } else if(ch==3){ // Ctrl-C
                    running=false; break;
                } else if(ch>=32 && ch<127){
                    input_buf.push_back((char)ch);
                    redraw();
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
#else
        TermRawGuard rg; rg.enable();
        for(;;){
            if(!running) break;
            unsigned char ch=0;
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if(n<=0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
            if(ch=='\r' || ch=='\n'){
                std::string out = input_buf;
                input_buf.clear();
                {
                    std::lock_guard<std::mutex> lk(io_mtx);
                    print_prompt_line(input_buf);
                }
                {
                    std::lock_guard<std::mutex> lk(q_mtx);
                    q.push_back(out);
                }
                q_cv.notify_one();
            } else if(ch==127 || ch==8){ // backspace
                if(!input_buf.empty()) input_buf.pop_back();
                redraw();
            } else if(ch==3){ // Ctrl-C
                running=false; break;
            } else if(ch>=32){
                input_buf.push_back((char)ch);
                redraw();
            }
        }
#endif
    });

    // Main sender loop consumes completed lines from queue
    bool want_reconnect=false;
    while(running){
        std::unique_lock<std::mutex> lk(q_mtx);
        q_cv.wait(lk, [&]{ return !q.empty() || !running; });
        if(!running) break;
        std::string input = std::move(q.front()); q.pop_front(); lk.unlock();

        input = trim(input);
        if(input.empty()) continue;

        if(input=="/dc"||input=="/disconnect"){ net::send_line(s, "QUIT"); running=false; break; }
        if(input=="/rc"||input=="/reconnect"){ net::send_line(s, "QUIT"); want_reconnect=true; running=false; break; }
        if(input=="/list"){ net::send_line(s, proto::make_msg("/list")); continue; }

        input = sanitize_message(input);
        net::send_line(s, proto::make_msg(input));
        // rx thread will show echo from server
    }

    if(rx.joinable()) rx.join();
    if(input_thr.joinable()) input_thr.join();
    net::clear_buffer(s);
    closesocket_cross(s);
    if(want_reconnect){ std::cout<<"\nReconnecting...\n"; return true; }
    return false;
}

static int menu_prompt(const std::string& title, const std::vector<MenuItem>& items){
    std::cout<<"\n=== "<<title<<" ===\n";
    for(auto& it: items){ std::cout<<it.idx<<") "<<it.label<<"\n"; }
    std::cout<<"> "; int c=-1; std::string line; std::getline(std::cin,line);
    try{ c=std::stoi(line);}catch(...){ c=-1;} return c;
}

int main(int argc, char** argv){
#ifndef _WIN32
    if(ensure_terminal_attached(argc, argv)) return 0;
#endif
    ClientConfig cfg; std::ifstream f("opicochat.cfg"); if(f.good()){
        std::stringstream ss; ss<<f.rdbuf(); f.close();
        std::ifstream f2("opicochat.cfg");
        Ini ini2; std::string ln; while(std::getline(f2,ln)){
            ln=trim(ln); if(ln.empty()||ln[0]=='#'||ln[0]==';'||ln[0]=='[') continue;
            auto pos = ln.find('='); if(pos==std::string::npos) continue;
            ini2.kv[trim(ln.substr(0,pos))]=trim(ln.substr(pos+1));
        }
        cfg = ClientConfig::from_ini(ini2);
        auto it = ini2.kv.find("remember_last_username");
        if(it != ini2.kv.end()) g_remember_last_username = (it->second=="1");
    }

    while(true){
        int c = menu_prompt("opico chat", {{1,"Connect to server"},{2,"Settings"},{3,"Quit"}});
        if(c==3) break;

        if(c==2){
            auto status = std::string(g_remember_last_username ? "ON" : "OFF");
            int sc = menu_prompt("Settings", {
                {0,"Back"},
                {1,"Add username (with optional hex #RRGGBB or RRGGBB)"},
                {2,"List usernames (colored)"},
                {3,"Add server (host[:port])"},
                {4,"List servers"},
                {5,std::string("Toggle 'Remember last username' (currently: ") + status + ")"},
            });
            if(sc==0){ /* back */ }
            else if(sc==1){
                if(cfg.usernames.size()>=10){ std::cout<<"Max 10 usernames.\n"; continue; }
                std::string name; std::cout<<"Username: "; std::getline(std::cin,name);
                std::string hex;  std::cout<<"Hex color (#RRGGBB or RRGGBB or blank): "; std::getline(std::cin,hex);
                hex = trim(hex);
                if(!hex.empty()){
                    std::string norm = normalize_hex_hash(hex);
                    if(norm.empty()){ std::cout<<"Invalid hex; ignoring color.\n"; hex.clear(); }
                    else hex = norm;
                }
                cfg.usernames.push_back({trim(name), hex});
                save_client_cfg(cfg);
            } else if(sc==2){
                if(cfg.usernames.empty()){ std::cout<<"(no usernames)\n"; continue; }
                for(size_t i=0;i<cfg.usernames.size();++i){
                    std::string preview = colorize_name(cfg.usernames[i].first, cfg.usernames[i].second);
                    std::cout<<(i+1)<<") "<<preview<<"\n";
                }
            } else if(sc==3){
                if(cfg.servers.size()>=10){ std::cout<<"Max 10 servers.\n"; continue; }
                std::string hp; std::cout<<"Host[:port] (default 24816): "; std::getline(std::cin,hp);
                std::string h; uint16_t p; if(parse_host_port(trim(hp),h,p,24816)) cfg.servers.push_back({h,p});
                save_client_cfg(cfg);
            } else if(sc==4){
                if(cfg.servers.empty()){ std::cout<<"(no servers)\n"; continue; }
                for(size_t i=0;i<cfg.servers.size();++i){ std::cout<<(i+1)<<") "<<cfg.servers[i].first<<":"<<cfg.servers[i].second<<"\n"; }
            } else if(sc==5){
                g_remember_last_username = !g_remember_last_username;
                std::cout<<"Remember last username is now "<<(g_remember_last_username?"ON":"OFF")<<".\n";
                save_client_cfg(cfg);
            }
        }
        else if(c==1){
            std::vector<MenuItem> items; items.push_back({0,"Back"}); items.push_back({1,"Manual: enter host[:port]"});
            int show_idx=2;
            for(size_t i=0;i<cfg.servers.size();++i){ items.push_back({show_idx++, cfg.servers[i].first+":"+std::to_string(cfg.servers[i].second)}); }
            int sc = menu_prompt("Connect", items);
            if(sc==0) continue;

            std::string host; uint16_t port=24816;
            if(sc==1){
                std::string hp; std::cout<<"Host[:port] (default 24816): "; std::getline(std::cin,hp);
                if(!parse_host_port(trim(hp), host, port, 24816)) continue;
            } else {
                int base=2; int idx = sc - base;
                if(idx<0 || (size_t)idx>=cfg.servers.size()) continue;
                host = cfg.servers[idx].first; port = cfg.servers[idx].second;
            }

            std::vector<MenuItem> uitems; uitems.push_back({0,"Back"}); uitems.push_back({1,"Type username"});
            int uidx=2;
            for(size_t i=0;i<cfg.usernames.size();++i){
                if(uidx==9) ++uidx;
                std::string preview = colorize_name(cfg.usernames[i].first, cfg.usernames[i].second);
                uitems.push_back({uidx++, preview});
            }
            if(g_remember_last_username && !cfg.last_username.empty()){
                uitems.push_back({9, std::string("Use last username (") + cfg.last_username + ")"});
            }
            int usc = menu_prompt("Username", uitems);
            if(usc==0) continue;

            std::string username; std::string hex;
            if(usc==1){
                std::cout<<"Username: "; std::getline(std::cin, username);
            } else if(usc==9 && g_remember_last_username && !cfg.last_username.empty()){
                username = cfg.last_username;
            } else {
                int selected = usc;
                if(selected<2) continue;
                if(selected>9) { selected -= 1; } // reserved 9
                int idx = selected - 2;
                if(idx<0 || (size_t)idx>=cfg.usernames.size()) continue;
                username = cfg.usernames[idx].first;
                hex      = cfg.usernames[idx].second;
            }

            if(username.empty()){ std::cout<<"Username required.\n"; continue; }

            cfg.last_host = host; cfg.last_port = port; cfg.last_username = username;
            if(g_remember_last_username) save_client_cfg(cfg);

            bool again=false; do { again = run_chat(host, port, username, hex); } while(again);
        }
    }
    return 0;
}
