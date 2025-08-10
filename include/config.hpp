#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <cstdint>

struct Ini {
    std::unordered_map<std::string, std::string> kv;
    bool has(const std::string& k) const { return kv.find(k)!=kv.end(); }
    std::string get(const std::string& k, const std::string& def="") const {
        auto it=kv.find(k); return it==kv.end()?def:it->second; }
    int get_int(const std::string& k, int def=0) const;
    bool get_bool(const std::string& k, bool def=false) const;
    void set(const std::string& k, const std::string& v){ kv[k]=v; }
    std::string serialize() const; // key=value per line
};

// Server config (opicochat.cfg)
struct ServerConfig {
    uint16_t port = 24816;
    std::string server_name = "[server]"; // default as per spec
    bool password_enabled = false;
    std::string password = "";
    int max_conn_per_ip = 5;
    int max_conn_per_sec_per_ip = 3;
    int max_total_conn = 200;
    std::string log_dir = "logs";

    static ServerConfig from_ini(const Ini& ini);
    static ServerConfig load_or_create(const std::string& path);
    static void write_default(const std::string& path, const ServerConfig& cfg);
};

// Client config (opicochat.cfg) – not generated automatically
struct ClientConfig {
    // up to 10 usernames with optional color hex #RRGGBB
    std::vector<std::pair<std::string,std::string>> usernames; // {name, hex}
    // up to 10 servers host:port
    std::vector<std::pair<std::string,uint16_t>> servers;
    std::string last_host=""; uint16_t last_port=24816; std::string last_username=""; 

    static ClientConfig from_ini(const Ini& ini);
    static Ini to_ini(const ClientConfig& cfg);
};
