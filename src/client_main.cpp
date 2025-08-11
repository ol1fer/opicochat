// opicochat - client_main.cpp (clean, nonblocking input + proper disconnect)

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
#include <iostream>
#include <fstream>

#include "common.hpp"
#include "config.hpp"
#include "protocol.hpp"
#include "utils.hpp"

using namespace std::chrono;

#ifdef _WIN32
#include <conio.h>
#else
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#endif

struct MenuItem { int idx; std::string label; };

static inline void print_prompt_line(const std::string& buf){
    std::cout << "\r\033[2K> " << buf << std::flush;
}

static bool g_remember_last_username = false;

// host:port key for admin-keys map
static std::string hp_key(const std::string& host, uint16_t port){
    return host + ":" + std::to_string(port);
}

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
}

#ifndef _WIN32
struct TermRawGuard {
    termios old{}; bool active=false;
    void enable(){
        if(active) return;
        tcgetattr(STDIN_FILENO, &old);
        termios raw = old;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;   // non-blocking when used with select
        raw.c_cc[VTIME]= 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        active=true;
    }
    void disable(){ if(!active) return; tcsetattr(STDIN_FILENO, TCSANOW, &old); active=false; }
    ~TermRawGuard(){ disable(); }
};
#endif

static bool run_chat(const std::string& host, uint16_t port,
                     const std::string& username,
                     const std::string& color_hex_pref,
                     const std::string& admin_key)
{
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
        }
        if(disconnected) break;
    }
    if(!got_welcome){ std::cout<<"No welcome from server\n"; closesocket_cross(s); return false; }

    // 2) AUTH (send normalized color or blank, plus optional admin key)
    std::string password;
    if(pass_required){ std::cout<<"Server requires a password: "; std::getline(std::cin, password); }
    std::string norm_hex = normalize_hex_hash(color_hex_pref); // "" if invalid/empty
    net::send_line(s, proto::make_auth(username, password, norm_hex, admin_key));
    if(!net::recv_line(s, line, 5000, disconnected)){ std::cout<<"No auth response\n"; closesocket_cross(s); return false; }
    if(line!="AUTH_OK"){ std::cout<<line<<"\n"; closesocket_cross(s); return false; }

    bool is_admin=false;
    std::deque<std::string> preloaded_lines;
    // Try to catch an immediate ADMIN_OK without waiting for the RX thread, and
    // also flush any early messages (MOTD, online list) into a small queue.
    {
        bool tmpdisc=false;
        if(net::recv_line(s, line, 200, tmpdisc)){
            if(line=="ADMIN_OK"){ is_admin=true; }
            else { preloaded_lines.push_back(line); }
            while(net::recv_line(s, line, 0, tmpdisc)) preloaded_lines.push_back(line);
        }
    }

    std::cout << "Connected to " << server_name << " as " << username;
    if(is_admin) std::cout << " (admin authenticated)";
    std::cout << ". Type /dc to disconnect or /help for more commands.\n";

    // IO state
    std::mutex io_mtx;
    std::string input_buf;

    // Flush any preloaded server lines before starting threads (maintain prompt)
    while(!preloaded_lines.empty()){
        std::string l = preloaded_lines.front(); preloaded_lines.pop_front();
        std::string iso,name,color,text;
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
    }

    std::atomic<bool> running{true};

    // Queue for complete input lines (so main thread can block on it)
    std::mutex q_mtx; std::condition_variable q_cv; std::deque<std::string> q;

    auto redraw = [&](){
        std::lock_guard<std::mutex> lk(io_mtx);
        print_prompt_line(input_buf);
    };

    // RX thread — prints messages, then redraws input line
    std::thread rx([&]{
        std::string iso,name,text,color; bool disc=false; std::string l;
        while(running){
            bool got=false;
            if(!preloaded_lines.empty()){ l=preloaded_lines.front(); preloaded_lines.pop_front(); got=true; }
            else if(net::recv_line(s, l, 500, disc)){ got=true; }

            if(got){
                if(l=="ADMIN_OK"){
                    is_admin = true;
                    std::lock_guard<std::mutex> lk(io_mtx);
                    std::cout << "\r\033[2K[server] admin privileges granted\n";
                    print_prompt_line(input_buf);
                    std::cout.flush();
                    continue;
                }
                if(l.rfind("ERROR ", 0)==0){
                    {
                        std::lock_guard<std::mutex> lk(io_mtx);
                        std::cout << "\r\033[2K" << l << "\n";
                        std::cout.flush();
                    }
                    running=false;
                    q_cv.notify_all();
                    break;
                }
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
                {
                    std::lock_guard<std::mutex> lk(io_mtx);
                    std::cout << "\r\033[2KDisconnected.\n";
                    std::cout.flush();
                }
                running=false;
                q_cv.notify_all();
                break;
            }
        }
    });

    // INPUT thread — single-line editor (non-blocking)
    std::thread input_thr([&]{
        #ifdef _WIN32
        for(;;){
            if(!running) break;
            if(_kbhit()){
                int ch = _getch();
                if(ch=='\r' || ch=='\n'){
                    std::string out = input_buf; input_buf.clear();
                    { std::lock_guard<std::mutex> lk(io_mtx); print_prompt_line(input_buf); }
                    { std::lock_guard<std::mutex> lk(q_mtx); q.push_back(out); }
                    q_cv.notify_one();
                } else if(ch==8 || ch==127){
                    if(!input_buf.empty()) input_buf.pop_back(); redraw();
                } else if(ch==3){ // Ctrl-C
                    running=false; q_cv.notify_all(); break;
                } else if(ch>=32 && ch<127){
                    input_buf.push_back((char)ch); redraw();
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        #else
        TermRawGuard rg; rg.enable();
        for(;;){
            if(!running) break;
            fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO,&rf);
            timeval tv{0, 100000}; // 100ms
            int r = select(STDIN_FILENO+1, &rf, nullptr, nullptr, &tv);
            if(!running) break;
            if(r<=0) continue;
            unsigned char ch=0; ssize_t n = read(STDIN_FILENO, &ch, 1);
            if(n<=0) continue;

            if(ch=='\r' || ch=='\n'){
                std::string out = input_buf; input_buf.clear();
                { std::lock_guard<std::mutex> lk(io_mtx); print_prompt_line(input_buf); }
                { std::lock_guard<std::mutex> lk(q_mtx); q.push_back(out); }
                q_cv.notify_one();
            } else if(ch==127 || ch==8){
                if(!input_buf.empty()) input_buf.pop_back(); redraw();
            } else if(ch==3){ // Ctrl-C
                running=false; q_cv.notify_all(); break;
            } else if(ch>=32){
                input_buf.push_back((char)ch); redraw();
            }
        }
        #endif
    });

    // Sender loop
    bool want_reconnect=false;
    while(running){
        std::unique_lock<std::mutex> lk(q_mtx);
        q_cv.wait(lk, [&]{ return !q.empty() || !running; });
        if(!running) break;
        std::string input = std::move(q.front()); q.pop_front(); lk.unlock();

        input = trim(input);
        if(input.empty()) continue;

        if(input=="/dc"||input=="/disconnect"){ net::send_line(s, "QUIT"); running=false; q_cv.notify_all(); break; }
        if(input=="/rc"||input=="/reconnect"){ net::send_line(s, "QUIT"); want_reconnect=true; running=false; q_cv.notify_all(); break; }
        if(input=="/list"){ net::send_line(s, proto::make_msg("/list")); continue; }

        input = sanitize_message(input);
        net::send_line(s, proto::make_msg(input));
    }

    if(rx.joinable()) rx.join();
    if(input_thr.joinable()) input_thr.join();
    net::clear_buffer(s);
    closesocket_cross(s);
    if(want_reconnect){ std::cout<<"\nReconnecting...\n"; return true; }
    return false;
}

// --- CLI menus -----

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
    ClientConfig cfg;
    {   // load config + remember flag if present
        std::ifstream f("opicochat.cfg");
        if(f.good()){
            std::ifstream f2("opicochat.cfg");
            Ini ini2; std::string ln;
            while(std::getline(f2,ln)){
                ln=trim(ln);
                if(ln.empty()||ln[0]=='#'||ln[0]==';'||ln[0]=='[') continue;
                auto pos = ln.find('='); if(pos==std::string::npos) continue;
                ini2.kv[trim(ln.substr(0,pos))]=trim(ln.substr(pos+1));
            }
            cfg = ClientConfig::from_ini(ini2);
            auto it = ini2.kv.find("remember_last_username");
            if(it != ini2.kv.end()) g_remember_last_username = (it->second=="1");
        }
    }

    auto connect_once = [&](const std::string& host, uint16_t port, const std::string& user, const std::string& hex)->bool{
        std::string key;
        auto it = cfg.admin_keys.find(hp_key(host,port));
        if(it != cfg.admin_keys.end()) key = it->second;
        return run_chat(host, port, user, hex, key);
    };

    // one-off connect that can pass an override admin key
    auto connect_once_with_key = [&](const std::string& host, uint16_t port,
                                     const std::string& user, const std::string& hex,
                                     const std::string& key_override)->bool{
        std::string key = key_override;
        if(key.empty()){
            auto it = cfg.admin_keys.find(hp_key(host,port));
            if(it != cfg.admin_keys.end()) key = it->second;
        }
        return run_chat(host, port, user, hex, key);
    };


    while(true){
        int c = menu_prompt("opico chat", {{1,"Connect to server"},{2,"Settings"},{3,"Quit"}});
        if(c==3) break;

        if(c==2){
            for(;;){
                int sc = menu_prompt("Settings", {
                    {0,"Back"},
                    {1,"Manage usernames"},
                    {2,"Manage servers"},
                });
if(sc==0) break;

                
if(sc==1){
    for(;;){
        std::vector<MenuItem> items; items.push_back({0,"Back"});
        for(size_t i=0;i<cfg.usernames.size();++i){
            std::string preview = colorize_name(cfg.usernames[i].first, cfg.usernames[i].second);
            items.push_back({(int)i+1, preview});
        }
        items.push_back({9,"Add new username"});

        int pick = menu_prompt("Manage usernames", items);
        if(pick==0) break;

        if(pick==9){
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
            continue;
        }

        int idx = pick-1;
        if(idx<0 || (size_t)idx>=cfg.usernames.size()) continue;

        int act = menu_prompt(std::string("Username '") + cfg.usernames[idx].first + "'", {
                    {0,"Back"},
                    {1,"Remove"}
                });
        if(act==1){
            cfg.usernames.erase(cfg.usernames.begin()+idx);
            save_client_cfg(cfg);
            std::cout<<"Removed.\n";
        }
    }
}

else if(sc==2){
    for(;;){
        std::vector<MenuItem> items; items.push_back({0,"Back"});
        for(size_t i=0;i<cfg.servers.size();++i){
            auto hp = hp_key(cfg.servers[i].first, cfg.servers[i].second);
            bool has_key = (cfg.admin_keys.find(hp)!=cfg.admin_keys.end());
            items.push_back({(int)i+1, cfg.servers[i].first+":"+std::to_string(cfg.servers[i].second) + (has_key? " [admin]" : "")});
        }
        items.push_back({9,"Add new server"});

        int pick = menu_prompt("Manage servers", items);
        if(pick==0) break;

        if(pick==9){
            if(cfg.servers.size()>=10){ std::cout<<"Max 10 servers.\n"; continue; }
            std::string hpstr; std::cout<<"Host[:port] (leave port blank for default): "; std::getline(std::cin,hpstr);
            std::string host; uint16_t port = 24816;
            if(!parse_host_port(trim(hpstr), host, port, 24816)) { std::cout<<"Invalid.\n"; continue; }
            cfg.servers.push_back({host,port});
            std::string key; std::cout<<"Admin key (leave blank if not admin): "; std::getline(std::cin,key);
            key = trim(key);
            if(!key.empty()) cfg.admin_keys[hp_key(host,port)] = key;
            save_client_cfg(cfg);
            continue;
        }

        int idx = pick-1;
        if(idx<0 || (size_t)idx>=cfg.servers.size()) continue;

        int act = menu_prompt(cfg.servers[idx].first+":"+std::to_string(cfg.servers[idx].second), {
            {0,"Back"},
            {1,"Remove"}
        });
        if(act==1){
            cfg.admin_keys.erase(hp_key(cfg.servers[idx].first, cfg.servers[idx].second));
            cfg.servers.erase(cfg.servers.begin()+idx);
            save_client_cfg(cfg);
            std::cout<<"Removed.\n";
        }
    }
}
else if(sc==3){

                    std::string hp; std::cout<<"Server host[:port]: "; std::getline(std::cin,hp);
                    std::string override_key; std::string host; uint16_t port=24816;
                    if(!parse_host_port(trim(hp), host, port, 24816)){ std::cout<<"Bad host/port.\n"; continue; }
                    std::string key; std::cout<<"(optional) Admin key for this server: "; std::getline(std::cin,key);
                    cfg.servers.push_back({host, port});
                    if(!trim(key).empty()) cfg.admin_keys[hp_key(host,port)] = trim(key);
                    save_client_cfg(cfg);
                }
                else if(sc==4){
                    if(cfg.servers.empty()){ std::cout<<"(no servers)\n"; continue; }
                    for(;;){
                        std::vector<MenuItem> items; items.push_back({0,"Back"});
                        for(size_t i=0;i<cfg.servers.size();++i){
                            items.push_back({(int)i+1, cfg.servers[i].first+":"+std::to_string(cfg.servers[i].second)});
                        }
                        int pick = menu_prompt("Servers", items);
                        if(pick==0) break;
                        int idx = pick-1;
                        if(idx<0 || (size_t)idx>=cfg.servers.size()) continue;

                        int act = menu_prompt(std::string("Server '")+cfg.servers[idx].first+":"+std::to_string(cfg.servers[idx].second)+"'", {
                            {0,"Back"},
                            {1,"Remove this server"}
                        });
                        if(act==1){
                            cfg.admin_keys.erase(hp_key(cfg.servers[idx].first, cfg.servers[idx].second));
                            cfg.servers.erase(cfg.servers.begin()+idx);
                            save_client_cfg(cfg);
                            std::cout<<"Removed.\n";
                        }
                    }
                }
                else if(sc==5){
                    g_remember_last_username = !g_remember_last_username;
                    if(!g_remember_last_username) cfg.last_username.clear();
                    save_client_cfg(cfg);
                    std::cout<<"Remember last username: "<<(g_remember_last_username?"ON":"OFF")<<"\n";
                }
            }
        }

        if(c==1){
            // pick a server
            std::string host = cfg.last_host; uint16_t port = cfg.last_port; std::string override_key;
            if(cfg.servers.empty()){
                std::string hp; std::cout<<"Server host[:port]: "; std::getline(std::cin,hp);
                if(!parse_host_port(trim(hp), host, port, 24816)){ std::cout<<"Bad host/port.\n"; continue; }
            } else {
                std::vector<MenuItem> items; items.push_back({0,"Back"});
                for(size_t i=0;i<cfg.servers.size();++i){
                    items.push_back({(int)i+1, cfg.servers[i].first+":"+std::to_string(cfg.servers[i].second)});
                }
                items.push_back({9,"Direct connect (one-off)"});
                int pick = menu_prompt("Choose server", items);
                if(pick==0) continue;
                int idx = pick-1; if(idx<0 || (size_t)idx>=cfg.servers.size()) continue;
                host = cfg.servers[idx].first; port = cfg.servers[idx].second;
            }

            // pick a username
            std::string user = (g_remember_last_username && !cfg.last_username.empty())? cfg.last_username : "";
            std::string hex  = "";
            if(cfg.usernames.empty()){
                if(user.empty()){ std::cout<<"Username: "; std::getline(std::cin,user); }
                std::cout<<"Hex color (#RRGGBB or RRGGBB or blank): "; std::getline(std::cin,hex);
                hex = normalize_hex_hash(trim(hex));
                // ...
            } else {
                std::vector<MenuItem> items; items.push_back({0,"Back"});
                for(size_t i=0;i<cfg.usernames.size();++i){
                    const std::string& name = cfg.usernames[i].first;
                    const std::string& hx   = cfg.usernames[i].second; // may be empty

                    // Show just the colored name (no hex text)
                    std::string label = colorize_name(name, hx);
                    // colorize_name() wraps with angle brackets; strip them for the menu label
                    if(label.size() >= 2 && label.front()=='<' && label.back()=='>')
                        label = label.substr(1, label.size()-2);

                    items.push_back({(int)i+1, std::move(label)});
                }
                items.push_back({9,"One-off username..."});
                int pick = menu_prompt("Choose username", items);
                if(pick==0) continue;
                if(pick==9){
                    std::cout<<"Username: "; std::getline(std::cin,user);
                    std::string hx; std::cout<<"Hex color (#RRGGBB or RRGGBB or blank): "; std::getline(std::cin,hx);
                    hx = trim(hx);
                    if(!hx.empty()){
                        std::string norm = normalize_hex_hash(hx);
                        if(norm.empty()){ std::cout<<"Invalid hex; ignoring color.\n"; }
                        else hex = norm;
                    }
                } else {
                    int idx = pick-1; if(idx<0 || (size_t)idx>=cfg.usernames.size()) continue;
                    user = cfg.usernames[idx].first; hex = cfg.usernames[idx].second;
                }
            }


            // try to connect (with reconnect loop if requested)
            for(;;){
                bool again = connect_once_with_key(host, port, user, hex, override_key);
                if(!again) break; // back to main menu
            }

            // remember last chosen
            cfg.last_host = host; cfg.last_port=port;
            if(g_remember_last_username) cfg.last_username = user;
            save_client_cfg(cfg);
        }
    }

    return 0;
}
