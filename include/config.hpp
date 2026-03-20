#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

struct Ini {
    std::unordered_map<std::string,std::string> kv;
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = kv.find(k); return it == kv.end() ? def : it->second;
    }
    void set(const std::string& k, const std::string& v) { kv[k] = v; }
    int         get_int (const std::string& k, int  def) const;
    bool        get_bool(const std::string& k, bool def) const;
    std::string serialize() const;
};

struct ServerConfig {
    uint16_t    port                  = 24816;
    std::string server_name           = "opico chat";
    std::string server_name_color     = "";       // #RRGGBB or empty
    std::string motd                  = "";
    bool        password_enabled      = false;
    std::string password              = "";
    int         max_conn_per_ip       = 5;
    int         max_conn_rate_per_min = 20;
    int         max_total_conn        = 200;
    std::string log_dir               = "logs";
    int         history_size          = 0;
    int         max_msg_per_window    = 6;
    int         msg_window_secs       = 4;
    int         keepalive_interval_secs = 30;     // seconds between keepalive pings
    int         keepalive_timeout_secs  = 10;     // seconds before kicking unresponsive client
    int         ip_block_duration_secs  = 300;    // how long to block an IP after rate-limit hit

    static ServerConfig from_ini(const Ini& ini);
    static void         write_default(const std::string& path, const ServerConfig& cfg);
    // Loads or creates the config file; appends any missing keys to existing files.
    static ServerConfig load_or_create(const std::string& path);
};

struct ClientConfig {
    std::string last_host;
    uint16_t    last_port     = 24816;
    std::string last_username;
    bool        show_timestamps    = true;
    bool        show_server_ip    = false;  // show host:port in server list
    bool        client_log_enabled = false; // write received chat to a local log file

    // {name, "#RRGGBB" or ""}
    std::vector<std::pair<std::string,std::string>> usernames;
    // {host, port}
    std::vector<std::pair<std::string,uint16_t>>    servers;
    // "host:port" -> admin_key
    std::unordered_map<std::string,std::string>     admin_keys;
    // "host:port" -> password
    std::unordered_map<std::string,std::string>     server_passwords;
    // "host:port" -> custom display name shown in server list
    std::unordered_map<std::string,std::string>     server_display_names;

    static ClientConfig from_ini(const Ini& ini);
    static Ini          to_ini(const ClientConfig& cfg);
};
