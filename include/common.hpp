#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <optional>
#include <mutex>
#include <atomic>
#include <algorithm>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t = SOCKET;
  inline void closesocket_cross(socket_t s){ closesocket(s);}  
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using socket_t = int;
  inline void closesocket_cross(socket_t s){ ::close(s);}  
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
#endif

namespace net {
struct Addr {
    std::string host; // hostname or ip
    uint16_t port{};
};

// Resolve hostname:port to a connected socket (client) or a listening socket (server)
socket_t connect_tcp(const std::string& host, uint16_t port, std::string& err);
socket_t listen_tcp(uint16_t port, std::string& err);
std::string peer_ip(socket_t s);

// Read/write line based protocol ("\n" delimited). Returns false on disconnect/error.
bool send_line(socket_t s, const std::string& line);
// Reads a line into out if available; nonblocking when timeout_ms == 0; returns true if a full line was read.
bool recv_line(socket_t s, std::string& out, int timeout_ms, bool& disconnected);
void clear_buffer(socket_t s);

// Utility
void set_nonblocking(socket_t s, bool nb);
}

// Simple ring buffer for last N messages
class RingBuffer {
public:
    explicit RingBuffer(size_t cap) : cap_(cap) {}
    void push(const std::string& s){ std::lock_guard<std::mutex> lk(m_); if(buf_.size()==cap_) buf_.erase(buf_.begin()); buf_.push_back(s);}    
    std::vector<std::string> snapshot() const { std::lock_guard<std::mutex> lk(m_); return buf_; }
private:
    size_t cap_;
    mutable std::mutex m_;
    std::vector<std::string> buf_;
};

// Rate limit per IP
struct RateLimiter {
    // connections per second per ip, and max concurrent per ip
    int max_conn_per_sec_per_ip = 3;
    int max_concurrent_per_ip = 10;

    // Track timestamps of accepts
    std::unordered_map<std::string, std::vector<std::chrono::steady_clock::time_point>> per_ip_times;
    std::unordered_map<std::string, int> concurrent_counts;

    bool allow_accept(const std::string& ip){
        using clk = std::chrono::steady_clock;
        auto now = clk::now();
        auto& vec = per_ip_times[ip];
        // prune to last 1s
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](auto& t){
            return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count() > 1000; }), vec.end());
        if ((int)vec.size() >= max_conn_per_sec_per_ip) return false;
        vec.push_back(now);
        int cc = ++concurrent_counts[ip];
        if (cc > max_concurrent_per_ip) { --concurrent_counts[ip]; return false; }
        return true;
    }
    void on_close(const std::string& ip){ if(concurrent_counts[ip]>0) concurrent_counts[ip]--; }
};

// Logging
std::string today_date();
std::string now_iso();
void ensure_dir(const std::string& path);
bool append_file(const std::string& path, const std::string& text);

