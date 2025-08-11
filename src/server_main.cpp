#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <cctype>
#include <chrono>

#include "common.hpp"
#include "config.hpp"
#include "protocol.hpp"
#include "utils.hpp"

#ifndef _WIN32
  #include <signal.h>
  #include <unistd.h>
  #include <termios.h>
  #include <sys/select.h>
#else
  #include <conio.h>
  #include <winsock2.h>
#endif

static inline void print_prompt_line(const std::string& buf){
    std::cout << "\r\033[2K> " << buf << std::flush;
}
static inline void srv_print_with_prompt(std::mutex& io_mtx, const std::string& line, const std::string& buf){
    std::lock_guard<std::mutex> lk(io_mtx);
    std::cout << "\r\033[2K" << line << "\n";
    print_prompt_line(buf);
}

#ifndef _WIN32
struct TermRawGuard {
    termios old{}; bool active=false;
    void enable(){
        if(active) return;
        tcgetattr(STDIN_FILENO, &old);
        termios raw = old;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME]= 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        active=true;
    }
    void disable(){ if(!active) return; tcsetattr(STDIN_FILENO, TCSANOW, &old); active=false; }
    ~TermRawGuard(){ disable(); }
};
#endif

static inline void srv_immediate_shutdown(socket_t s){
#ifdef _WIN32
    ::shutdown(s, SD_BOTH);
#else
    ::shutdown(s, SHUT_RDWR);
#endif
}

// ---------- Admin DB ----------
struct AdminEntry { std::string original_user; std::string last_user; };
static std::unordered_map<std::string, AdminEntry> g_admins; // key -> entry

static void load_admins(const std::string& path){
    g_admins.clear();
    std::ifstream f(path);
    if(!f.good()) return;
    std::string line;
    while(std::getline(f,line)){
        line = trim(line); if(line.empty()||line[0]=='#') continue;
        std::stringstream ss(line);
        std::string key,orig,last;
        if(!(ss>>key>>orig>>last)) continue;
        g_admins[key] = {orig,last};
    }
}
static void save_admins(const std::string& path){
    std::ofstream f(path);
    for(auto& kv: g_admins){
        f<<kv.first<<" "<<kv.second.original_user<<" "<<kv.second.last_user<<"\n";
    }
}
static std::string random_key(size_t n=32){
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0,61);
    std::string s; s.reserve(n);
    for(size_t i=0;i<n;++i) s.push_back(a[dist(rng)]);
    return s;
}

// ---------- Bans DB ----------
static std::unordered_map<std::string,std::string> g_ban_users; // username -> reason (legacy)
static std::unordered_map<std::string,std::string> g_ban_ips;   // ip -> reason

static void load_bans(const std::string& path){
    g_ban_users.clear(); g_ban_ips.clear();
    std::ifstream f(path);
    if(!f.good()) return;
    std::string line;
    while(std::getline(f,line)){
        line = trim(line); if(line.empty()||line[0]=='#') continue;
        auto p = line.find('=');
        if(p==std::string::npos) continue;
        std::string k = line.substr(0,p), v=line.substr(p+1);
        if(k.rfind("user:",0)==0) g_ban_users[k.substr(5)]=v;
        else if(k.rfind("ip:",0)==0) g_ban_ips[k.substr(3)]=v;
    }
}
static void save_bans(const std::string& path){
    std::ofstream f(path);
    for(auto& kv: g_ban_users) f<<"user:"<<kv.first<<"="<<kv.second<<"\n";
    for(auto& kv: g_ban_ips)   f<<"ip:"<<kv.first  <<"="<<kv.second<<"\n";
}

static std::string render_pretty_colored(const std::string& line){
    std::string iso,name,text,color;
    if(!proto::parse_chat(line, iso,name,color,text)) return line;
    std::string token = colorize_name(name, color=="-"? "" : color);
    return token + " " + text;
}

struct ClientInfo {
    socket_t s{};
    std::string ip;
    std::string username;
    std::string color_hex;
    std::string admin_key;
    bool authed=false;
    bool is_admin=false;
    bool was_kicked=false;
    bool was_banned=false;
};

static bool file_exists(const std::string& p){ std::ifstream f(p); return f.good(); }
static std::string unique_log_path(const std::string& dir, const std::string& date){
    std::string base = dir + "/" + date + ".log";
    if(!file_exists(base)) return base;
    for(int i=1;i<10000;i++){
        std::string p = dir + "/" + date + "-" + std::to_string(i) + ".log";
        if(!file_exists(p)) return p;
    }
    return base;
}

int main(int argc, char** argv){
#ifdef _WIN32
    enable_windows_ansi();
#else
    if(ensure_terminal_attached(argc, argv)) return 0;
    signal(SIGPIPE, SIG_IGN);
#endif

    auto cfg = ServerConfig::load_or_create("opicochatserver.cfg");
    ensure_dir(cfg.log_dir);

    load_admins("admins.cfg");
    load_bans("banned.cfg");

    std::string err; socket_t lsock = net::listen_tcp(cfg.port, err);
    if (lsock==INVALID_SOCKET){ std::cerr<<"Listen failed: "<<err<<"\n"; return 1; }
    std::cout << "opico chat server listening on port "<<cfg.port<<". Logs: "<<cfg.log_dir<<"\n";

    std::string cur_date = today_date();
    std::string current_log = unique_log_path(cfg.log_dir, cur_date);

    std::vector<ClientInfo> clients; clients.reserve(64);
    // Per-minute sliding window of accepted connections per IP
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> ip_accept_minute;

    std::atomic<bool> running{true};

    std::mutex cmd_mtx; std::deque<std::function<void()>> cmd_q;
    auto post_cmd = [&](std::function<void()> fn){ std::lock_guard<std::mutex> lk(cmd_mtx); cmd_q.push_back(std::move(fn)); };

    std::mutex io_mtx;
    std::string input_buf;

    auto srv_log_cmd = [&](const std::string& who, const std::string& raw){
        append_file(current_log, now_iso()+" [cmd] "+who+" ran: "+raw+"\n");
    };

    std::thread console([&]{
#ifdef _WIN32
        for(;;){
            if(!running) break;
            if(_kbhit()){
                int ch = _getch();
                if(ch=='\r' || ch=='\n'){
                    std::string raw = trim(input_buf); input_buf.clear();
                    print_prompt_line(input_buf);
                    if(raw.empty()) continue;

                    if(raw[0]=='/'){
                        post_cmd([&, raw]{
                            srv_log_cmd("[server]", raw);
                            auto next = [&](std::istringstream& ss){ std::string t; ss>>t; return t; };
                            std::istringstream ss(raw);
                            std::string slash; ss>>slash;

                            if(slash=="/help"){
                                std::string h =
                                    "/help  #this help\n"
                                    "/list  #list users\n"
                                    "/lookup <user>        #IP & admin\n"
                                    "/kick <user> [reason] #kick\n"
                                    "/ban <user> [reason]  #ban by user IP\n"
                                    "/banip <ip> [reason]  #ban IP\n"
                                    "/unban <ip>           #remove IP ban\n"
                                    "/banlist              #list banned IPs\n"
                                    "/reload bans|admins   #reload files\n"
                                    "/admin add|remove <user> | /admin list";
                                srv_print_with_prompt(io_mtx, h, input_buf);
                                return;
                            } else if(slash=="/admin"){
                                std::string sub = next(ss);
                                if(sub=="add"){
                                    std::string user = next(ss);
                                    if(user.empty()){ srv_print_with_prompt(io_mtx, "Usage: /admin add <username>", input_buf); return; }
                                    std::string key = random_key(32);
                                    g_admins[key] = {user,user}; save_admins("admins.cfg");
                                    for(auto& c: clients){ if(c.authed && c.username==user){ 
net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "ADMIN KEY: " + key)); } }
                                    srv_print_with_prompt(io_mtx, "Admin key generated for "+user+": "+key, input_buf);
                                } else if(sub=="remove"){
                                    std::string user = next(ss);
                                    if(user.empty()){ srv_print_with_prompt(io_mtx, "Usage: /admin remove <username>", input_buf); return; }
                                    std::string remove_key="";
                                    for(auto& c: clients) if(c.authed && c.username==user && !c.admin_key.empty()) { remove_key=c.admin_key; break; }
                                    if(remove_key.empty()){
                                        for(auto& kv: g_admins){ if(kv.second.original_user==user || kv.second.last_user==user){ remove_key=kv.first; break; } }
                                    }
                                    if(remove_key.empty()){ srv_print_with_prompt(io_mtx, "No admin key found for "+user, input_buf); return; }
                                    g_admins.erase(remove_key); save_admins("admins.cfg");
                                    srv_print_with_prompt(io_mtx, "Removed admin for "+user, input_buf);
                                } else if(sub=="list"){
                                    std::ostringstream os; os<<"Admins:";
                                    for(auto& kv: g_admins) os<<"\n  orig="<<kv.second.original_user<<", last="<<kv.second.last_user;
                                    srv_print_with_prompt(io_mtx, os.str(), input_buf);
                                } else {
                                    srv_print_with_prompt(io_mtx, "Usage: /admin [add|remove|list]", input_buf);
                                }
                            } else if(slash=="/list"){
                                int cnt=0; std::string names;
                                for(auto& u: clients){ if(u.authed){ if(cnt++) names+=", "; names+=u.username; } }
                                srv_print_with_prompt(io_mtx, "Users ("+std::to_string(cnt)+"): "+names, input_buf);
                            } else if(slash=="/lookup"){
                                std::string user = next(ss);
                                if(user.empty()){ srv_print_with_prompt(io_mtx, "Usage: /lookup <username>", input_buf); return; }
                                bool found=false;
                                for(auto& u: clients){
                                    if(u.authed && u.username==user){
                                        srv_print_with_prompt(io_mtx, user+" -> IP "+u.ip+", admin: "+(u.is_admin?"yes":"no"), input_buf);
                                        found=true; break;
                                    }
                                }
                                if(!found) srv_print_with_prompt(io_mtx, "No such user: "+user, input_buf);
                            } else if(slash=="/kick"){
                                std::string user = next(ss); std::string reason; std::getline(ss,reason); reason=trim(reason); if(reason.empty()) reason="You were kicked.";
                                bool found=false;
                                for(auto& c: clients){
                                    if(c.authed && c.username==user){
                                        c.was_kicked=true;
                                        net::send_line(c.s, "ERROR "+reason);
                                        srv_immediate_shutdown(c.s);
                                        found=true; break;
                                    }
                                }
                                srv_print_with_prompt(io_mtx, found? ("Kicked "+user) : ("No such user: "+user), input_buf);
                            } else if(slash=="/ban"){
                                std::string user = next(ss); std::string reason; std::getline(ss,reason); reason=trim(reason); if(reason.empty()) reason="You are banned.";
                                if(user.empty()){ srv_print_with_prompt(io_mtx, "Usage: /ban <username> [reason]", input_buf); return; }
                                bool found=false; std::string ip;
                                for(auto& c: clients){
                                    if(c.authed && c.username==user){
                                        ip = c.ip; c.was_banned=true;
                                        net::send_line(c.s, "ERROR "+reason);
                                        srv_immediate_shutdown(c.s);
                                        found=true; break;
                                    }
                                }
                                if(found){
                                    g_ban_ips[ip]=reason; save_bans("banned.cfg");
                                    srv_print_with_prompt(io_mtx, "Banned "+user+" (IP "+ip+")", input_buf);
                                } else {
                                    srv_print_with_prompt(io_mtx, "No such user: "+user, input_buf);
                                }
                            } else if(slash=="/banip"){
                                std::string ip = next(ss); std::string reason; std::getline(ss,reason); reason=trim(reason); if(reason.empty()) reason="You are banned.";
                                if(ip.empty()){ srv_print_with_prompt(io_mtx, "Usage: /banip <ip> [reason]", input_buf); return; }
                                g_ban_ips[ip]=reason; save_bans("banned.cfg");
                                for(auto& c: clients){ if(c.ip==ip){ c.was_banned=true; net::send_line(c.s, "ERROR "+reason); srv_immediate_shutdown(c.s); } }
                                srv_print_with_prompt(io_mtx, "Banned IP "+ip, input_buf);
                            } else if(slash=="/reload"){
                                std::string what = next(ss);
                                if(what=="bans"){ load_bans("banned.cfg"); srv_print_with_prompt(io_mtx, "Reloaded bans.", input_buf); }
                                else if(what=="admins"){ load_admins("admins.cfg"); srv_print_with_prompt(io_mtx, "Reloaded admins.", input_buf); }
                                else srv_print_with_prompt(io_mtx, "Usage: /reload [bans|admins]", input_buf);
                            } else if(slash=="/unban"){
                                std::string ip = next(ss);
                                if(ip.empty()){ srv_print_with_prompt(io_mtx, "Usage: /unban <ip>", input_buf); }
                                else {
                                    size_t n = g_ban_ips.erase(ip);
                                    save_bans("banned.cfg");
                                    srv_print_with_prompt(io_mtx, n? ("Unbanned IP "+ip):("IP not found: "+ip), input_buf);
                                }
                            } else if(slash=="/banlist"){
                                std::ostringstream os; os<<"Banned IPs:";
                                for(auto& kv: g_ban_ips) os<<"\n  "<<kv.first<<"  reason: "<<kv.second;
                                srv_print_with_prompt(io_mtx, os.str(), input_buf);
                            } else {
                                srv_print_with_prompt(io_mtx, "Unknown command.", input_buf);
                            }
                        });
                    } else {
                        post_cmd([&, raw]{
                            std::string msg = sanitize_message(raw);
                            std::string full = proto::make_chat(now_iso(), "[server]", "-", msg);
                            for(auto& c: clients) if(c.authed) net::send_line(c.s, full);
                            append_file(current_log, full+"\n");
                            srv_print_with_prompt(io_mtx, render_pretty_colored(full), input_buf);
                        });
                    }
                } else if(ch==8 || ch==127){
                    if(!input_buf.empty()) input_buf.pop_back();
                    print_prompt_line(input_buf);
                } else if(ch==3){ running=false; break; }
                else if(ch>=32 && ch<127){ input_buf.push_back((char)ch); print_prompt_line(input_buf); }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
#else
        TermRawGuard rg; rg.enable();
        for(;;){
            if(!running) break;
            fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO,&rf);
            timeval tv{0, 100000};
            int r = select(STDIN_FILENO+1, &rf, nullptr, nullptr, &tv);
            if(!running) break;
            if(r<=0) continue;

            unsigned char ch=0; ssize_t n = read(STDIN_FILENO, &ch, 1);
            if(n<=0) continue;

            if(ch=='\r' || ch=='\n'){
                std::string raw = trim(input_buf); input_buf.clear();
                print_prompt_line(input_buf);
                if(raw.empty()) continue;

                if(raw[0]=='/'){
                    post_cmd([&, raw]{
                        srv_log_cmd("[server]", raw);
                        auto next = [&](std::istringstream& ss){ std::string t; ss>>t; return t; };
                        std::istringstream ss(raw);
                        std::string slash; ss>>slash;

                        if(slash=="/help"){
                            std::string h =
                                "/help  #this help\n"
                                "/list  #list users\n"
                                "/lookup <user>        #IP & admin\n"
                                "/kick <user> [reason] #kick\n"
                                "/ban <user> [reason]  #ban by user IP\n"
                                "/banip <ip> [reason]  #ban IP\n"
                                "/unban <ip>           #remove IP ban\n"
                                "/banlist              #list banned IPs\n"
                                "/reload bans|admins   #reload files\n"
                                "/admin add|remove <user> | /admin list";
                            srv_print_with_prompt(io_mtx, h, input_buf);
                        } else if(slash=="/admin"){
                            std::string sub = next(ss);
                            if(sub=="add"){
                                std::string user = next(ss);
                                if(user.empty()){ srv_print_with_prompt(io_mtx, "Usage: /admin add <username>", input_buf); return; }
                                std::string key = random_key(32);
                                g_admins[key] = {user,user}; save_admins("admins.cfg");
                                for(auto& c: clients){ if(c.authed && c.username==user){ 
net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "ADMIN KEY: " + key)); } }
                                srv_print_with_prompt(io_mtx, "Admin key generated for "+user+": "+key, input_buf);
                            } else if(sub=="remove"){
                                std::string user = next(ss);
                                if(user.empty()){ srv_print_with_prompt(io_mtx, "Usage: /admin remove <username>", input_buf); return; }
                                std::string remove_key="";
                                for(auto& c: clients) if(c.authed && c.username==user && !c.admin_key.empty()) { remove_key=c.admin_key; break; }
                                if(remove_key.empty()){
                                    for(auto& kv: g_admins){ if(kv.second.original_user==user || kv.second.last_user==user){ remove_key=kv.first; break; } }
                                }
                                if(remove_key.empty()){ srv_print_with_prompt(io_mtx, "No admin key found for "+user, input_buf); return; }
                                g_admins.erase(remove_key); save_admins("admins.cfg");
                                srv_print_with_prompt(io_mtx, "Removed admin for "+user, input_buf);
                            } else if(sub=="list"){
                                std::ostringstream os; os<<"Admins:";
                                for(auto& kv: g_admins) os<<"\n  orig="<<kv.second.original_user<<", last="<<kv.second.last_user;
                                srv_print_with_prompt(io_mtx, os.str(), input_buf);
                            } else {
                                srv_print_with_prompt(io_mtx, "Usage: /admin [add|remove|list]", input_buf);
                            }
                        } else if(slash=="/list"){
                            int cnt=0; std::string names;
                            for(auto& u: clients){ if(u.authed){ if(cnt++) names+=", "; names+=u.username; } }
                            srv_print_with_prompt(io_mtx, "Users ("+std::to_string(cnt)+"): "+names, input_buf);
                        } else if(slash=="/lookup"){
                            std::string user = next(ss);
                            if(user.empty()){ srv_print_with_prompt(io_mtx, "Usage: /lookup <username>", input_buf); return; }
                            bool found=false;
                            for(auto& u: clients){
                                if(u.authed && u.username==user){
                                    srv_print_with_prompt(io_mtx, user+" -> IP "+u.ip+", admin: "+(u.is_admin?"yes":"no"), input_buf);
                                    found=true; break;
                                }
                            }
                            if(!found) srv_print_with_prompt(io_mtx, "No such user: "+user, input_buf);
                        } else if(slash=="/kick"){
                            std::string user = next(ss); std::string reason; std::getline(ss,reason); reason=trim(reason); if(reason.empty()) reason="You were kicked.";
                            bool found=false;
                            for(auto& c: clients){
                                if(c.authed && c.username==user){
                                    c.was_kicked=true;
                                    net::send_line(c.s, "ERROR "+reason);
                                    srv_immediate_shutdown(c.s);
                                    found=true; break;
                                }
                            }
                            srv_print_with_prompt(io_mtx, found? ("Kicked "+user) : ("No such user: "+user), input_buf);
                        } else if(slash=="/ban"){
                            std::string user = next(ss); std::string reason; std::getline(ss,reason); reason=trim(reason); if(reason.empty()) reason="You are banned.";
                            if(user.empty()){ srv_print_with_prompt(io_mtx, "Usage: /ban <username> [reason]", input_buf); return; }
                            bool found=false; std::string ip;
                            for(auto& c: clients){
                                if(c.authed && c.username==user){
                                    ip = c.ip; c.was_banned=true;
                                    net::send_line(c.s, "ERROR "+reason);
                                    srv_immediate_shutdown(c.s);
                                    found=true; break;
                                }
                            }
                            if(found){
                                g_ban_ips[ip]=reason; save_bans("banned.cfg");
                                srv_print_with_prompt(io_mtx, "Banned "+user+" (IP "+ip+")", input_buf);
                            } else {
                                srv_print_with_prompt(io_mtx, "No such user: "+user, input_buf);
                            }
                        } else if(slash=="/banip"){
                            std::string ip = next(ss); std::string reason; std::getline(ss,reason); reason=trim(reason); if(reason.empty()) reason="You are banned.";
                            if(ip.empty()){ srv_print_with_prompt(io_mtx, "Usage: /banip <ip> [reason]", input_buf); return; }
                            g_ban_ips[ip]=reason; save_bans("banned.cfg");
                            for(auto& c: clients){ if(c.ip==ip){ c.was_banned=true; net::send_line(c.s, "ERROR "+reason); srv_immediate_shutdown(c.s); } }
                            srv_print_with_prompt(io_mtx, "Banned IP "+ip, input_buf);
                        } else if(slash=="/reload"){
                            std::string what = next(ss);
                            if(what=="bans"){ load_bans("banned.cfg"); srv_print_with_prompt(io_mtx, "Reloaded bans.", input_buf); }
                            else if(what=="admins"){ load_admins("admins.cfg"); srv_print_with_prompt(io_mtx, "Reloaded admins.", input_buf); }
                            else srv_print_with_prompt(io_mtx, "Usage: /reload [bans|admins]", input_buf);
                        } else if(slash=="/unban"){
                            std::string ip = next(ss);
                            if(ip.empty()){ srv_print_with_prompt(io_mtx, "Usage: /unban <ip>", input_buf); }
                            else {
                                size_t n = g_ban_ips.erase(ip);
                                save_bans("banned.cfg");
                                srv_print_with_prompt(io_mtx, n? ("Unbanned IP "+ip):("IP not found: "+ip), input_buf);
                            }
                        } else if(slash=="/banlist"){
                            std::ostringstream os; os<<"Banned IPs:";
                            for(auto& kv: g_ban_ips) os<<"\n  "<<kv.first<<"  reason: "<<kv.second;
                            srv_print_with_prompt(io_mtx, os.str(), input_buf);
                        } else {
                            srv_print_with_prompt(io_mtx, "Unknown command.", input_buf);
                        }
                    });
                } else {
                    post_cmd([&, raw]{
                        std::string msg = sanitize_message(raw);
                        std::string full = proto::make_chat(now_iso(), "[server]", "-", msg);
                        for(auto& c: clients) if(c.authed) net::send_line(c.s, full);
                        append_file(current_log, full+"\n");
                        srv_print_with_prompt(io_mtx, render_pretty_colored(full), input_buf);
                    });
                }
            } else if(ch==127 || ch==8){
                if(!input_buf.empty()) input_buf.pop_back();
                print_prompt_line(input_buf);
            } else if(ch==3){ running=false; break; }
            else if(ch>=32){ input_buf.push_back((char)ch); print_prompt_line(input_buf); }
        }
#endif
    });

    auto broadcast_join = [&](const std::string& user){
        std::string joinmsg = proto::make_chat(now_iso(), "[server]", "-", user + " joined");
        for(auto& c: clients) if(c.authed) net::send_line(c.s, joinmsg);
        append_file(current_log, joinmsg+"\n");
        srv_print_with_prompt(io_mtx, render_pretty_colored(joinmsg), input_buf);
    };

    while(running){
        std::string want_date = today_date();
        if(want_date!=cur_date){ cur_date=want_date; current_log = unique_log_path(cfg.log_dir, cur_date); }

        {
            std::deque<std::function<void()>> tmp;
            { std::lock_guard<std::mutex> lk(cmd_mtx); tmp.swap(cmd_q); }
            for(auto& fn : tmp) fn();
        }

        fd_set rf; FD_ZERO(&rf); FD_SET(lsock, &rf);
        socket_t maxfd = lsock;
        for(auto& c: clients){ FD_SET(c.s, &rf); if(c.s>maxfd) maxfd=c.s; }
        timeval tv{0, 200*1000};
        int r = select((int)(maxfd+1), &rf, nullptr, nullptr, &tv);
        if(r<0){ continue; }

        if (FD_ISSET(lsock, &rf)){
            sockaddr_storage ss{}; socklen_t slen=sizeof(ss);
            socket_t cs = accept(lsock, (sockaddr*)&ss, &slen);
            if (cs!=INVALID_SOCKET){
                std::string ip; char host[NI_MAXHOST];
                if(getnameinfo((sockaddr*)&ss,slen,host,sizeof(host),nullptr,0,NI_NUMERICHOST)==0) ip=host; else ip="";
                if((int)clients.size() >= cfg.max_total_conn){ net::send_line(cs, "ERROR Server full"); closesocket_cross(cs); }
                
else {
                                // Concurrent authed connections from this IP
                int conc = 0; for (auto &u : clients) if (u.authed && u.ip == ip) ++conc;
                if (conc >= cfg.max_conn_per_ip) { net::send_line(cs, "ERROR Too many concurrent connections from your IP"); closesocket_cross(cs); continue; }

                // Per-minute accepted-connections window (repurpose max_conn_per_sec_per_ip as per-minute)
                auto &dq = ip_accept_minute[ip];
                auto now = std::chrono::steady_clock::now();
                while (!dq.empty() && (now - dq.front()) > std::chrono::minutes(1)) dq.pop_front();
                if ((int)dq.size() >= cfg.max_conn_per_sec_per_ip) { net::send_line(cs, "ERROR Rate limited"); closesocket_cross(cs); continue; }
                dq.push_back(now);

                    ClientInfo ci; ci.s=cs; ci.ip=ip;
                    net::send_line(ci.s, proto::make_welcome(cfg.server_name, cfg.password_enabled));
                    clients.push_back(ci);
                }
            }
        }

        for(size_t i=0;i<clients.size();){
            auto& c = clients[i]; bool disc=false; std::string line;

            while(net::recv_line(c.s, line, 0, disc)){
                if(line.rfind("AUTH ",0)==0){
                    std::string user, pass, color, key; bool has_color=false, has_key=false;
                    if(!proto::parse_auth(line, user, pass, color, has_color, key, has_key)) { net::send_line(c.s, "AUTH_FAIL Bad auth format"); disc=true; break; }
                    user = trim(user);
                    if(!is_valid_username(user)) { net::send_line(c.s, "AUTH_FAIL Invalid username (A-Z, a-z, 0-9, _)"); disc=true; break; }

                    if(auto itbip = g_ban_ips.find(c.ip); itbip!=g_ban_ips.end()){ net::send_line(c.s, "ERROR "+itbip->second); disc=true; break; }

                    bool dup=false; for(auto& u: clients){ if(&u!=&c && u.authed && u.username==user){ dup=true; break; } }
                    if(dup){ net::send_line(c.s, "AUTH_FAIL Username already in use"); disc=true; break; }

                    if(cfg.password_enabled && pass!=cfg.password){ net::send_line(c.s, "AUTH_FAIL Wrong password"); disc=true; break; }

                    if(auto itbu = g_ban_users.find(user); itbu!=g_ban_users.end()){ net::send_line(c.s, "ERROR "+itbu->second); disc=true; break; }

                    
if (has_color) { std::string norm = normalize_hex_hash(color); if (norm.empty()) { c.color_hex.clear(); } else { c.color_hex = norm; } } else { c.color_hex.clear(); }


                    c.username = user; c.authed = true;

                    if(has_key){
                        if(auto it = g_admins.find(key); it!=g_admins.end()){
                            c.is_admin = true; c.admin_key = key;
                            it->second.last_user = user; save_admins("admins.cfg");
                        }
                    }

                    // >>> FIX: explicit auth success handshake <<<
                    net::send_line(c.s, "AUTH_OK");
if(c.is_admin) net::send_line(c.s, "ADMIN_OK");
{
    int online=0; for(auto& u: clients){ if(u.authed) ++online; }
    std::string label = (online==1? "user" : "users");
    net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", std::to_string(online) + " " + label + " currently online"));
    if(!cfg.motd.empty()) net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", cfg.motd));
}
srv_print_with_prompt(io_mtx, std::string("[server] ")+user+" connected from "+c.ip, input_buf);
                    broadcast_join(user);
                }
                else if(line.rfind("MSG ",0)==0){
                    if(!c.authed) { net::send_line(c.s, "ERROR Not authed"); continue; }
                    std::string text; if(!proto::parse_msg(line, text)) continue; text = sanitize_message(text);

                    if(!text.empty() && text[0]=='/'){
                        srv_log_cmd(c.username, text);
                        try { srv_print_with_prompt(io_mtx, std::string("[cmd] ")+c.username+" ran: "+text, input_buf); } catch(...) {}

                        auto next = [&](std::istringstream& ss){ std::string t; ss>>t; return t; };
                        std::istringstream ss(text);
                        std::string slash; ss>>slash;

                        if(slash=="/help"){
                            const char* h_user[] = {
                                "/help  #show this",
                                "/list  #list users",
                                "/dc|/disconnect  #disconnect",
                                "/rc|/reconnect   #reconnect",
                                nullptr
                            };
                            const char* h_admin[] = {
                                "/lookup <user>        #IP & admin (admin)",
                                "/kick <user> [reason] #kick (admin)",
                                "/ban <user> [reason]  #ban by user IP (admin)",
                                "/banip <ip> [reason]  #ban IP (admin)",
                                "/unban <ip>           #remove IP ban (admin)",
                                "/banlist              #list banned IPs (admin)",
                                "/reload bans|admins   #reload files (admin)",
                                "/admin add|remove <user> | /admin list  #admins (admin)",
                                nullptr
                            };
                            for(int i=0; h_user[i]; ++i)
                                net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", h_user[i]));
                            if(c.is_admin)
                                for(int i=0; h_admin[i]; ++i)
                                    net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", h_admin[i]));
                            continue;
                        }

                        if(slash=="/list"){
                            std::string names; int cnt=0; for(auto& u: clients){ if(u.authed){ if(cnt++) names += ", "; names += u.username; }}
                            std::string reply = proto::make_chat(now_iso(), "[server]", "-", "Users ("+std::to_string(cnt)+"): " + names);
                            net::send_line(c.s, reply);
                            continue;
                        }

                        if(!c.is_admin){
                            net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Not an admin."));
                            continue;
                        }

                        if(slash=="/admin"){
                            std::string sub = next(ss);
                            if(sub=="add"){
                                std::string user = next(ss);
                                if(user.empty()){ net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Usage: /admin add <username>")); continue; }
                                std::string key = random_key(32);
                                g_admins[key] = {user,user}; save_admins("admins.cfg");
                                bool sent=false;
                                for(auto& u: clients){ if(u.authed && u.username==user){ net::send_line(u.s, "CHAT " + now_iso() + " [server] - ADMIN KEY: " + key); sent=true; } }
                                net::send_line(c.s, "CHAT " + now_iso() + " [server] - ADMIN KEY for "+user+": " + key + (sent? "":" (user not online)"));
                            } else if(sub=="remove"){
                                std::string user = next(ss);
                                if(user.empty()){ net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Usage: /admin remove <username>")); continue; }
                                std::string remove_key="";
                                for(auto& u: clients) if(u.authed && u.username==user && !u.admin_key.empty()) { remove_key=u.admin_key; break; }
                                if(remove_key.empty()){
                                    for(auto& kv: g_admins){ if(kv.second.original_user==user || kv.second.last_user==user){ remove_key=kv.first; break; } }
                                }
                                if(remove_key.empty()){ net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "No admin key found for "+user)); continue; }
                                g_admins.erase(remove_key); save_admins("admins.cfg");
                                net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Removed admin for "+user));
                            } else if(sub=="list"){
                                std::string out="Admins: ";
                                bool first=true; for(auto& kv: g_admins){ if(!first) out += "; "; first=false; out += kv.second.original_user + " (last: " + kv.second.last_user + ")"; }
                                net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", out));
                            } else {
                                net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Usage: /admin [add|remove|list] ..."));
                            }
                            continue;
                        } else if(slash=="/lookup"){
                            std::string user = next(ss);
                            if(user.empty()){ net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Usage: /lookup <username>")); continue; }
                            bool found=false;
                            for(auto& u: clients){
                                if(u.authed && u.username==user){
                                    std::string out = user + " -> IP " + u.ip + ", admin: " + (u.is_admin? "yes":"no");
                                    net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", out));
                                    found=true; break;
                                }
                            }
                            if(!found) net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "No such user: "+user));
                            continue;
                        } else if(slash=="/kick"){
                            std::string user = next(ss); std::string reason; std::getline(ss,reason); reason=trim(reason); if(reason.empty()) reason="You were kicked.";
                            bool found=false;
                            for(auto& u: clients){
                                if(u.authed && u.username==user){
                                    u.was_kicked=true;
                                    net::send_line(u.s, "ERROR "+reason);
                                    srv_immediate_shutdown(u.s);
                                    found=true; break;
                                }
                            }
                            net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", found? ("Kicked "+user) : ("No such user: "+user)));
                            continue;
                        } else if(slash=="/ban"){
                            std::string user = next(ss); std::string reason; std::getline(ss,reason); reason=trim(reason); if(reason.empty()) reason="You are banned.";
                            if(user.empty()){ net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Usage: /ban <username> [reason]")); continue; }
                            bool found=false; std::string ip;
                            for(auto& u: clients){
                                if(u.authed && u.username==user){
                                    ip = u.ip; u.was_banned=true;
                                    net::send_line(u.s, "ERROR "+reason);
                                    srv_immediate_shutdown(u.s);
                                    found=true; break;
                                }
                            }
                            if(found){
                                g_ban_ips[ip]=reason; save_bans("banned.cfg");
                                net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Banned "+user+" (IP "+ip+")"));
                                srv_print_with_prompt(io_mtx, "Banned "+user+" (IP "+ip+")", input_buf);
                            } else {
                                net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "No such user: "+user));
                            }
                            continue;
                        } else if(slash=="/banip"){
                            std::string ip = next(ss); std::string reason; std::getline(ss,reason); reason=trim(reason); if(reason.empty()) reason="You are banned.";
                            if(ip.empty()){ net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Usage: /banip <ip> [reason]")); continue; }
                            g_ban_ips[ip]=reason; save_bans("banned.cfg");
                            for(auto& u: clients){ if(u.ip==ip){ u.was_banned=true; net::send_line(u.s, "ERROR "+reason); srv_immediate_shutdown(u.s); } }
                            net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Banned IP "+ip));
                            srv_print_with_prompt(io_mtx, "Banned IP "+ip, input_buf);
                            continue;
                        } else if(slash=="/reload"){
                            std::string what = next(ss);
                            if(what=="bans"){
                                load_bans("banned.cfg");
                                net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Reloaded bans."));
                            } else if(what=="admins"){
                                load_admins("admins.cfg");
                                net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Reloaded admins."));
                            } else {
                                net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Usage: /reload [bans|admins]"));
                            }
                            continue;
                        } else if(slash=="/unban"){
                            std::string ip = next(ss);
                            if(ip.empty()){
                                net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Usage: /unban <ip>"));
                            } else {
                                size_t n = g_ban_ips.erase(ip);
                                save_bans("banned.cfg");
                                net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", n? ("Unbanned IP "+ip):("IP not found: "+ip)));
                            }
                            continue;
                        } else if(slash=="/banlist"){
                            std::ostringstream os; os<<"Banned IPs: ";
                            bool first=true; for(auto& kv: g_ban_ips){ if(!first) os << " ; "; first=false; os<<kv.first; }
                            net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", os.str()));
                            continue;
                        } else {
                            net::send_line(c.s, proto::make_chat(now_iso(), "[server]", "-", "Unknown command."));
                            continue;
                        }
                    } else {
                        std::string full = proto::make_chat(now_iso(), c.username, c.color_hex.empty()?"-":c.color_hex, text);
                        for(auto& u: clients) if(u.authed) net::send_line(u.s, full);
                        append_file(current_log, full+"\n");
                        srv_print_with_prompt(io_mtx, render_pretty_colored(full), input_buf);
                    }
                }
            }

            if(disc){
                net::clear_buffer(c.s);
                closesocket_cross(c.s);
                std::string leftmsg;
                if(c.was_banned){
                    leftmsg = proto::make_chat(now_iso(), "[server]", "-", c.username + " was banned from the server");
                } else if(c.was_kicked){
                    leftmsg = proto::make_chat(now_iso(), "[server]", "-", c.username + " was kicked from the server");
                } else if(c.authed){
                    leftmsg = proto::make_chat(now_iso(), "[server]", "-", c.username + " left");
                }
                if(!leftmsg.empty()){
                    for(auto& u: clients) if(&u!=&c && u.authed) net::send_line(u.s, leftmsg);
                    append_file(current_log, leftmsg+"\n");
                    srv_print_with_prompt(io_mtx, render_pretty_colored(leftmsg), input_buf);
                }
                clients.erase(clients.begin()+i);
                continue;
            }
            ++i;
        }
    }

    if(console.joinable()) console.join();
    for(auto& c: clients){ net::clear_buffer(c.s); closesocket_cross(c.s); }
    closesocket_cross(lsock);
    return 0;
}
