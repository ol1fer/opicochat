#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <direct.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <fcntl.h>
  #include <unistd.h>
#endif

#include <ctime>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <sys/stat.h>
#include "common.hpp"

namespace net {

// Thread-local recv buffers — one per socket per thread.
// Sockets must be read from a single thread. Use take/seed to hand off.
static thread_local std::unordered_map<socket_t, std::string> t_bufs;

socket_t connect_tcp(const std::string& host, uint16_t port, std::string& err) {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    std::string ps = std::to_string(port);
    if(getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0) {
        err = "dns resolution failed for: " + host;
        return INVALID_SOCKET;
    }
    socket_t sock = INVALID_SOCKET;
    for(auto* p = res; p; p = p->ai_next) {
        sock = (socket_t)::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if(sock == INVALID_SOCKET) continue;
        if(::connect(sock, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket_cross(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if(sock == INVALID_SOCKET) err = "connection refused or timed out";
    return sock;
}

socket_t listen_tcp(uint16_t port, std::string& err) {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    std::string ps = std::to_string(port);
    if(getaddrinfo(nullptr, ps.c_str(), &hints, &res) != 0) {
        err = "getaddrinfo failed";
        return INVALID_SOCKET;
    }
    socket_t lsock = INVALID_SOCKET;
    for(auto* p = res; p; p = p->ai_next) {
        lsock = (socket_t)::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if(lsock == INVALID_SOCKET) continue;
        if(p->ai_family == AF_INET6) {
#ifdef _WIN32
            DWORD v6only = 0;
            setsockopt(lsock, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
#else
            int v6only = 0;
            setsockopt(lsock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
        }
#ifdef _WIN32
        BOOL excl = TRUE;
        setsockopt(lsock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl));
#else
        int opt = 1;
        setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
#endif
        if(::bind(lsock, p->ai_addr, (int)p->ai_addrlen) == 0 &&
           ::listen(lsock, SOMAXCONN) == 0) break;
        closesocket_cross(lsock);
        lsock = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if(lsock == INVALID_SOCKET) err = "bind/listen failed on port " + std::to_string(port);
    return lsock;
}

bool send_line(socket_t s, const std::string& line, crypto::CipherStream* cs) {
    std::string data = (cs && cs->active ? cs->encrypt_line(line) : line) + "\n";
    const char* p = data.c_str();
    size_t left = data.size();
    while(left > 0) {
#ifdef _WIN32
        int n = ::send(s, p, (int)left, 0);
#else
        ssize_t n = ::send(s, p, left, 0);
#endif
        if(n <= 0) return false;
        left -= (size_t)n;
        p    += n;
    }
    return true;
}

bool recv_line(socket_t s, std::string& out, int timeout_ms, bool& disconnected,
               crypto::CipherStream* cs) {
    disconnected = false;
    std::string& buf = t_bufs[s];

    // fast path: already have a complete line buffered
    {
        auto pos = buf.find('\n');
        if(pos != std::string::npos) {
            out = buf.substr(0, pos);
            if(!out.empty() && out.back() == '\r') out.pop_back();
            buf.erase(0, pos + 1);
            if(cs && cs->active) out = cs->decrypt_line(out);
            return true;
        }
    }

    fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
#ifdef _WIN32
    int nfds = (int)(s + 1);
#else
    int nfds = s + 1;
#endif
    timeval tv{};
    timeval* ptv = nullptr;
    if(timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    int r = select(nfds, &rf, nullptr, nullptr, ptv);
    if(r <= 0) return false;

    char tmp[8192];
#ifdef _WIN32
    int n = ::recv(s, tmp, (int)sizeof(tmp), 0);
#else
    ssize_t n = ::recv(s, tmp, sizeof(tmp), 0);
#endif
    if(n <= 0) { disconnected = true; return false; }

    buf.append(tmp, tmp + n);
    auto pos = buf.find('\n');
    if(pos == std::string::npos) return false;

    out = buf.substr(0, pos);
    if(!out.empty() && out.back() == '\r') out.pop_back();
    buf.erase(0, pos + 1);
    if(cs && cs->active) { out = cs->decrypt_line(out); }
    return true;
}

std::string take_recv_buffer(socket_t s) {
    auto it = t_bufs.find(s);
    if(it == t_bufs.end()) return "";
    std::string result = std::move(it->second);
    t_bufs.erase(it);
    return result;
}

void seed_recv_buffer(socket_t s, std::string data) {
    if(!data.empty()) t_bufs[s] = std::move(data);
}

void clear_buffer(socket_t s) { t_bufs.erase(s); }

void set_nonblocking(socket_t s, bool nb) {
#ifdef _WIN32
    u_long mode = nb ? 1 : 0; ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0); if(flags < 0) flags = 0;
    fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

} // namespace net

std::string today_date() {
    std::time_t t = std::time(nullptr); std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

std::string now_iso() {
    // Always UTC so clients in different timezones can convert to their local time
    std::time_t t = std::time(nullptr); std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::string format_hhmm(const std::string& iso) {
    // Parse "YYYY-MM-DDTHH:MM:SS" (UTC) and convert to local HH:MM
    if(iso.size() < 16 || iso[10] != 'T') return "";
    std::tm utc_tm{};
    try {
        utc_tm.tm_year = std::stoi(iso.substr(0,4)) - 1900;
        utc_tm.tm_mon  = std::stoi(iso.substr(5,2)) - 1;
        utc_tm.tm_mday = std::stoi(iso.substr(8,2));
        utc_tm.tm_hour = std::stoi(iso.substr(11,2));
        utc_tm.tm_min  = std::stoi(iso.substr(14,2));
        utc_tm.tm_sec  = (iso.size() >= 19) ? std::stoi(iso.substr(17,2)) : 0;
    } catch(...) { return iso.substr(11, 5); }
    utc_tm.tm_isdst = 0;
#ifdef _WIN32
    std::time_t t = _mkgmtime(&utc_tm);
#else
    std::time_t t = timegm(&utc_tm);
#endif
    if(t == (std::time_t)-1) return iso.substr(11, 5);
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &t);
#else
    localtime_r(&t, &local_tm);
#endif
    char buf[6];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
    return buf;
}

void ensure_dir(const std::string& path) {
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

bool append_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::app);
    if(!f) return false;
    f << text;
    return true;
}
