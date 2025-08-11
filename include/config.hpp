#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

// Simple INI key/value holder
struct Ini {
    std::unordered_map<std::string,std::string> kv;

    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    }
    void set(const std::string& k, const std::string& v){
        kv[k] = v;
    }

    // implemented in config.cpp
    int         get_int (const std::string& k, int  def) const;
    bool        get_bool(const std::string& k, bool def) const;
    std::string serialize() const;
};

// -------- Server side config --------
struct ServerConfig {
    uint16_t    port                      = 24816;
    std::string server_name               = "[server]";
    std::string motd                      = "";       // Message of the day (optional)
    bool        password_enabled          = false;
    std::string password                  = "";
    int         max_conn_per_ip           = 5;
    int         max_conn_per_sec_per_ip   = 3;
    int         max_total_conn            = 200;
    std::string log_dir                   = "logs";

    static ServerConfig from_ini(const Ini& ini);
    static void         write_default(const std::string& path, const ServerConfig& cfg);
    static ServerConfig load_or_create(const std::string& path);
};

// -------- Client side config --------
struct ClientConfig {
    std::string last_host;
    uint16_t    last_port = 24816;
    std::string last_username;

    // usernames: vector of {name, optionalColorHexWithLeadingHash}
    std::vector<std::pair<std::string,std::string>> usernames;

    // servers: vector of {host, port}
    std::vector<std::pair<std::string,uint16_t>>    servers;

    // Per-server admin keys: "host:port" -> "KEY"
    std::unordered_map<std::string,std::string>     admin_keys;

    static ClientConfig from_ini(const Ini& ini);
    static Ini          to_ini(const ClientConfig& cfg);
};
