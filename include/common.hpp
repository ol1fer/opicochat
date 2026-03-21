#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <chrono>
#include "crypto.hpp"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t = SOCKET;
  inline void closesocket_cross(socket_t s){ closesocket(s); }
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using socket_t = int;
  inline void closesocket_cross(socket_t s){ ::close(s); }
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
#endif

namespace net {

socket_t connect_tcp(const std::string& host, uint16_t port, std::string& err);
socket_t listen_tcp(uint16_t port, std::string& err);

// Line-based I/O ("\n" delimited, strips "\r").
// Each socket should be read from ONE thread only (buffers are thread-local).
// Use take_recv_buffer / seed_recv_buffer to hand off between threads.
// Pass cs != nullptr to encrypt/decrypt with ChaCha20.
bool send_line(socket_t s, const std::string& line, crypto::CipherStream* cs = nullptr);
bool recv_line(socket_t s, std::string& out, int timeout_ms, bool& disconnected,
               crypto::CipherStream* cs = nullptr);

// Transfer leftover buffered bytes when handing socket from one thread to another.
std::string take_recv_buffer(socket_t s);
void        seed_recv_buffer(socket_t s, std::string data);

void clear_buffer(socket_t s);
void set_nonblocking(socket_t s, bool nb);

} // namespace net

// Ring buffer for recent message history (thread-safe)
class RingBuffer {
public:
    explicit RingBuffer(size_t cap) : cap_(cap) {}
    void push(const std::string& s){
        std::lock_guard<std::mutex> lk(m_);
        if(cap_ == 0) return;
        if(buf_.size() == cap_) buf_.erase(buf_.begin());
        buf_.push_back(s);
    }
    std::vector<std::string> snapshot() const {
        std::lock_guard<std::mutex> lk(m_);
        return buf_;
    }
    void reset(size_t new_cap) {
        std::lock_guard<std::mutex> lk(m_);
        cap_ = new_cap; buf_.clear();
    }
private:
    size_t cap_;
    mutable std::mutex m_;
    std::vector<std::string> buf_;
};

// Logging helpers
std::string today_date();
std::string now_iso();
std::string format_hhmm(const std::string& iso); // "HH:MM" from ISO timestamp
void ensure_dir(const std::string& path);
bool append_file(const std::string& path, const std::string& text);
