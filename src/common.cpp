#ifdef _WIN32
// Order matters: winsock2 before windows.h (we don't include windows.h here, but be safe)
#include <winsock2.h>
#include <ws2tcpip.h>  // addrinfo, getaddrinfo, IPV6_V6ONLY
#include <direct.h>    // _mkdir
#pragma comment(lib, "ws2_32.lib")
#else
  #include <fcntl.h>    // fcntl, O_NONBLOCK
  #include <unistd.h>
#endif

#include <ctime>
#include "common.hpp"
#include <cstring>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <unordered_map>
#include <string>

namespace net {

static thread_local std::unordered_map<socket_t, std::string> g_buffers;

socket_t connect_tcp(const std::string& host, uint16_t port, std::string& err){
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    std::string portstr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portstr.c_str(), &hints, &res)!=0){
        err = "DNS resolution failed";
        return INVALID_SOCKET;
    }

    socket_t sock = INVALID_SOCKET;
    for (addrinfo* p=res; p; p=p->ai_next){
        sock = (socket_t)::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        if (::connect(sock, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket_cross(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (sock==INVALID_SOCKET) err = "connect failed";
    return sock;
}

socket_t listen_tcp(uint16_t port, std::string& err){
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res=nullptr;
    std::string portstr = std::to_string(port);
    if (getaddrinfo(nullptr, portstr.c_str(), &hints, &res)!=0){
        err = "getaddrinfo failed";
        return INVALID_SOCKET;
    }

    socket_t lsock = INVALID_SOCKET;
    for (addrinfo* p=res; p; p=p->ai_next){
        lsock = (socket_t)::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (lsock == INVALID_SOCKET) continue;

        // Make IPv6 socket dual-stack so it accepts IPv4-mapped connections too.
        if (p->ai_family == AF_INET6){
#ifdef _WIN32
            DWORD v6only = 0;
            setsockopt(lsock, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
#else
            int v6only = 0;
            setsockopt(lsock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
        }

        int opt=1;
        setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
        if (::bind(lsock, p->ai_addr, (int)p->ai_addrlen)==0 && ::listen(lsock, SOMAXCONN)==0) break;

        closesocket_cross(lsock);
        lsock = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (lsock==INVALID_SOCKET) err = "listen/bind failed";
    return lsock;
}

std::string peer_ip(socket_t s){
    sockaddr_storage ss{}; socklen_t len = sizeof(ss);
    if (getpeername(s, (sockaddr*)&ss, &len)!=0) return "";
    char host[NI_MAXHOST];
    if (getnameinfo((sockaddr*)&ss, len, host, sizeof(host), nullptr, 0, NI_NUMERICHOST)!=0) return "";
    return host;
}

bool send_line(socket_t s, const std::string& line){
    std::string data = line + "\n";
    const char* p = data.c_str();
    size_t left = data.size();
    while(left>0){
#ifdef _WIN32
        int n = ::send(s, p, (int)left, 0);
#else
        ssize_t n = ::send(s, p, left, 0);
#endif
        if (n<=0) return false;
        left -= (size_t)n;
        p    += n;
    }
    return true;
}

bool recv_line(socket_t s, std::string& out, int timeout_ms, bool& disconnected){
    disconnected = false;
    std::string& buf = g_buffers[s];

    fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
#ifdef _WIN32
    int nfds = (int)(s + 1);
#else
    int nfds = s + 1;
#endif

    timeval tv{}; timeval* ptv = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }

    int r = select(nfds, &rf, nullptr, nullptr, ptv);
    if (r <= 0) return false;

    char tmp[4096];
#ifdef _WIN32
    int n = ::recv(s, tmp, (int)sizeof(tmp), 0);
#else
    ssize_t n = ::recv(s, tmp, sizeof(tmp), 0);
#endif
    if (n <= 0) { disconnected = true; return false; }

    buf.append(tmp, tmp + n);
    auto pos = buf.find('\n');
    if (pos == std::string::npos) return false;

    out = buf.substr(0, pos);
    if (!out.empty() && out.back() == '\r') out.pop_back();
    buf.erase(0, pos + 1);
    return true;
}

void clear_buffer(socket_t s){ g_buffers.erase(s); }

void set_nonblocking(socket_t s, bool nb){
#ifdef _WIN32
    u_long mode = nb?1:0; ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0); if(flags<0) flags=0;
    fcntl(s, F_SETFL, nb? (flags|O_NONBLOCK):(flags & ~O_NONBLOCK));
#endif
}

} // namespace net

std::string today_date(){
    std::time_t t = std::time(nullptr); std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm,&t);
#else
    localtime_r(&t,&tm);
#endif
    char buf[16];
    std::strftime(buf,sizeof(buf),"%Y-%m-%d", &tm);
    return buf;
}

std::string now_iso(){
    std::time_t t = std::time(nullptr); std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm,&t);
#else
    localtime_r(&t,&tm);
#endif
    char buf[32];
    std::strftime(buf,sizeof(buf),"%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

void ensure_dir(const std::string& path){
#ifdef _WIN32
    _mkdir(path.c_str());     // ok if exists
#else
    mkdir(path.c_str(), 0755);
#endif
}

bool append_file(const std::string& path, const std::string& text){
    std::ofstream f(path, std::ios::app);
    if(!f) return false;
    f << text;
    return true;
}
