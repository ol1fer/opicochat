#include <atomic>
#include <sys/stat.h>
#include "common.hpp"
#include "config.hpp"
#include "protocol.hpp"
#include "utils.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <unordered_map>
#include <set>
#include <fstream>

struct ClientInfo {
    socket_t s;
    std::string ip;
    std::string username;
    std::string color_hex;  // normalized "#RRGGBB" or empty
    bool authed=false;
};

static bool file_exists(const std::string& p){ std::ifstream f(p); return f.good(); }
static std::string unique_log_path(const std::string& dir, const std::string& date){
    std::string base = dir + "/" + date + ".log";
    if(!file_exists(base)) return base;
    for(int i=1;i<10000;i++){
        std::string p = dir + "/" + date + "-" + std::to_string(i) + ".log";
        if(!file_exists(p)) return p;
    }
    return base; // fallback
}

static std::string render_pretty_colored(const std::string& line){
    std::string iso,name,text,color;
    if(!proto::parse_chat(line, iso,name,color,text)) return line;
    // colorize_name handles [server] pink + white brackets automatically
    std::string token = colorize_name(name, color=="-"? "" : color);
    return token + " " + text;
}

int main(int argc, char** argv){
#ifdef _WIN32
    enable_windows_ansi();
#else
    if(ensure_terminal_attached(argc, argv)) return 0;
#endif
    auto cfg = ServerConfig::load_or_create("opicochatserver.cfg");
    ensure_dir(cfg.log_dir);

    std::string err; socket_t lsock = net::listen_tcp(cfg.port, err);
    if (lsock==INVALID_SOCKET){ std::cerr<<"Listen failed: "<<err<<"\n"; return 1; }
    std::cout << "opico chat server listening on port "<<cfg.port<<". Logs: "<<cfg.log_dir<<"\n";

    RingBuffer last50(50);
    std::string cur_date = today_date();
    std::string current_log = unique_log_path(cfg.log_dir, cur_date);

    std::vector<ClientInfo> clients; clients.reserve(64);
    RateLimiter rl; rl.max_conn_per_sec_per_ip = cfg.max_conn_per_sec_per_ip; rl.max_concurrent_per_ip = cfg.max_conn_per_ip;

    std::atomic<bool> running{true};

    // stdin reader thread for server chat
    std::thread console([&]{
        std::string line;
        while(running && std::getline(std::cin, line)){
            line = trim(line); if(line.empty()) continue;
            std::string msg = sanitize_message(line);
            std::string full = proto::make_chat(now_iso(), cfg.server_name, "-", msg);
            std::cout << "\033[1A\033[2K\r"; std::cout.flush();
            for(auto& c: clients){ if(c.authed) net::send_line(c.s, full); }
            last50.push(full);
            append_file(current_log, full+"\n");
            std::cout<<render_pretty_colored(full)<<"\n";
        }
    });

    auto broadcast_join = [&](const std::string& user){
        std::string joinmsg = proto::make_chat(now_iso(), cfg.server_name, "-", user + " joined");
        for(auto& c: clients){ if(c.authed) net::send_line(c.s, joinmsg); }
        last50.push(joinmsg); append_file(current_log, joinmsg+"\n"); std::cout<<render_pretty_colored(joinmsg)<<"\n";
    };
    auto broadcast_left = [&](const std::string& user){
        std::string left = proto::make_chat(now_iso(), cfg.server_name, "-", user + " left");
        for(auto& c: clients){ if(c.authed) net::send_line(c.s, left); }
        last50.push(left); append_file(current_log, left+"\n"); std::cout<<render_pretty_colored(left)<<"\n";
    };

    while(running){
        // rotate log by date (with uniqueness)
        std::string want_date = today_date();
        if(want_date!=cur_date){ cur_date=want_date; current_log = unique_log_path(cfg.log_dir, cur_date); }

        fd_set rf; FD_ZERO(&rf); FD_SET(lsock, &rf); socket_t maxfd = lsock;
        for(auto& c: clients){ FD_SET(c.s, &rf); if(c.s>maxfd) maxfd=c.s; }
        timeval tv{0, 200*1000};
        int r = select((int)(maxfd+1), &rf, nullptr, nullptr, &tv);
        if (r<0){ break; }
        if (FD_ISSET(lsock, &rf)){
            sockaddr_storage ss{}; socklen_t slen=sizeof(ss);
            socket_t cs = accept(lsock, (sockaddr*)&ss, &slen);
            if (cs!=INVALID_SOCKET){
                std::string ip; char host[NI_MAXHOST]; if(getnameinfo((sockaddr*)&ss,slen,host,sizeof(host),nullptr,0,NI_NUMERICHOST)==0) ip=host; else ip="";
                if((int)clients.size() >= cfg.max_total_conn){ net::send_line(cs, "ERROR Server full"); closesocket_cross(cs); }
                else if(!rl.allow_accept(ip)) { net::send_line(cs, "ERROR Rate limited"); closesocket_cross(cs); }
                else {
                    ClientInfo ci{cs, ip, "", "", false};
                    net::send_line(ci.s, proto::make_welcome(cfg.server_name, cfg.password_enabled));
                    clients.push_back(ci);
                }
            }
        }
        // client traffic
        for(size_t i=0;i<clients.size();){
            auto& c = clients[i]; bool disc=false; std::string line;
            while(net::recv_line(c.s, line, 0, disc)){
                if(line.rfind("AUTH ",0)==0){
                    std::string user, pass, color; bool has_color=false;
                    if(!proto::parse_auth(line, user, pass, color, has_color)) { net::send_line(c.s, "AUTH_FAIL Bad auth format"); disc=true; break; }
                    user = trim(user);
                    if(!is_valid_username(user)) { net::send_line(c.s, "AUTH_FAIL Invalid username (A-Z, a-z, 0-9, _)"); disc=true; break; }
                    if(cfg.password_enabled && pass!=cfg.password){ net::send_line(c.s, "AUTH_FAIL Wrong password"); disc=true; break; }
                    if(has_color){
                        std::string norm = normalize_hex_hash(color);
                        if(norm.empty()){ net::send_line(c.s, "AUTH_FAIL Invalid color hex"); disc=true; break; }
                        c.color_hex = norm;
                    } else c.color_hex.clear();
                    c.username = user; c.authed = true; net::send_line(c.s, "AUTH_OK"); broadcast_join(user);
                } else if(line.rfind("MSG ",0)==0){
                    if(!c.authed) { net::send_line(c.s, "ERROR Not authed"); continue; }
                    std::string text; if(!proto::parse_msg(line, text)) continue; text = sanitize_message(text);
                    if(text=="/list"){
                        std::string names; int cnt=0; for(auto& u: clients){ if(u.authed){ if(cnt++) names += ", "; names += u.username; }}
                        std::string reply = proto::make_chat(now_iso(), cfg.server_name, "-", "Users ("+std::to_string(cnt)+"): " + names);
                        net::send_line(c.s, reply);
                        continue;
                    }
                    std::string full = proto::make_chat(now_iso(), c.username, c.color_hex.empty()? "-" : c.color_hex, text);
                    for(auto& other: clients){ if(other.authed) net::send_line(other.s, full);}
                    last50.push(full); append_file(current_log, full+"\n");
                    std::cout<<render_pretty_colored(full)<<"\n";
                } else if(line=="QUIT"){
                    disc=true; break;
                } else {
                    // ignore unknown
                }
            }
            if(disc){
                if(c.authed) broadcast_left(c.username);
                rl.on_close(c.ip); net::clear_buffer(c.s); closesocket_cross(c.s); clients.erase(clients.begin()+i);
            }
            else ++i;
        }
    }

    running=false;
    for(auto& c: clients) closesocket_cross(c.s);
    closesocket_cross(lsock);
    return 0;
}
