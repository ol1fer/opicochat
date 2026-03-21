#include "config.hpp"
#include "utils.hpp"
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cctype>

static bool file_exists_cfg(const std::string& p) { std::ifstream f(p); return f.good(); }

static Ini read_ini_file(const std::string& path) {
    Ini ini;
    std::ifstream f(path);
    if(!f.good()) return ini;
    std::string line;
    while(std::getline(f, line)) {
        line = trim(line);
        if(line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        auto p = line.find('=');
        if(p == std::string::npos) continue;
        std::string k = trim(line.substr(0, p));
        std::string v = trim(line.substr(p + 1));
        if(!k.empty()) ini.set(k, v);
    }
    return ini;
}

int Ini::get_int(const std::string& k, int def) const {
    auto it = kv.find(k); if(it == kv.end()) return def;
    try { return std::stoi(it->second); } catch(...) { return def; }
}

bool Ini::get_bool(const std::string& k, bool def) const {
    auto it = kv.find(k); if(it == kv.end()) return def;
    std::string v = it->second;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    if(v=="1"||v=="true" ||v=="yes"||v=="on")  return true;
    if(v=="0"||v=="false"||v=="no" ||v=="off") return false;
    return def;
}

std::string Ini::serialize() const {
    std::string s;
    for(const auto& kvp : kv) { s += kvp.first; s += '='; s += kvp.second; s += '\n'; }
    return s;
}

ServerConfig ServerConfig::from_ini(const Ini& ini) {
    ServerConfig c;
    c.port               = (uint16_t)ini.get_int("port", 24816);
    c.server_name        = ini.get("server_name", "opico chat");
    c.server_name_color  = ini.get("server_name_color", "");
    c.motd               = ini.get("motd", "");
    c.password_enabled   = ini.get_bool("password_enabled", false);
    c.password           = ini.get("password", "");
    c.max_conn_per_ip    = ini.get_int("max_concurrent_per_ip", 5);
    c.max_conn_rate_per_min = ini.get_int("max_conn_per_min_per_ip", 20);
    c.max_total_conn     = ini.get_int("max_total_conn", 200);
    c.log_dir            = ini.get("log_dir", "logs");
    c.history_size       = ini.get_int("history_size", 50);
    c.max_msg_per_window = ini.get_int("max_msg_per_window", 6);
    c.msg_window_secs    = ini.get_int("msg_window_secs", 4);
    c.keepalive_interval_secs = ini.get_int("keepalive_interval_secs", 30);
    c.keepalive_timeout_secs  = ini.get_int("keepalive_timeout_secs",  10);
    c.ip_block_duration_secs  = ini.get_int("ip_block_duration_secs",  300);
    c.allow_version_mismatch  = ini.get_bool("allow_version_mismatch", false);
    c.banned_names            = ini.get("banned_names", "admin,mod");
    c.motd_color              = ini.get("motd_color", "");
    return c;
}

void ServerConfig::write_default(const std::string& path, const ServerConfig& c) {
    std::ofstream f(path);
    f << "# opicochat server config\n";
    f << "port="                    << c.port                  << "\n";
    f << "server_name="             << c.server_name           << "\n";
    f << "server_name_color="       << c.server_name_color     << "\n";
    f << "motd="                    << c.motd                  << "\n";
    f << "password_enabled="        << (c.password_enabled?"1":"0") << "\n";
    f << "password="                << c.password              << "\n";
    f << "max_concurrent_per_ip="   << c.max_conn_per_ip       << "\n";
    f << "max_conn_per_min_per_ip=" << c.max_conn_rate_per_min << "\n";
    f << "max_total_conn="          << c.max_total_conn        << "\n";
    f << "log_dir="                 << c.log_dir               << "\n";
    f << "history_size="            << c.history_size          << "\n";
    f << "max_msg_per_window="      << c.max_msg_per_window    << "\n";
    f << "msg_window_secs="         << c.msg_window_secs       << "\n";
    f << "keepalive_interval_secs=" << c.keepalive_interval_secs << "\n";
    f << "keepalive_timeout_secs="  << c.keepalive_timeout_secs  << "\n";
    f << "ip_block_duration_secs="  << c.ip_block_duration_secs  << "\n";
    f << "allow_version_mismatch="  << (c.allow_version_mismatch ? "1" : "0") << "\n";
    f << "banned_names="            << c.banned_names            << "\n";
    f << "motd_color="              << c.motd_color              << "\n";
}

void ServerConfig::save(const std::string& path, const ServerConfig& cfg) {
    write_default(path, cfg);
}

// Appends any config keys that are missing from an existing config file.
static void append_missing_server_keys(const std::string& path, const Ini& existing) {
    ServerConfig def;
    std::ofstream f(path, std::ios::app);
    auto add = [&](const std::string& k, const std::string& v) {
        if(existing.kv.find(k) == existing.kv.end())
            f << k << "=" << v << "\n";
    };
    add("port",                    std::to_string(def.port));
    add("server_name",             def.server_name);
    add("server_name_color",       def.server_name_color);
    add("motd",                    def.motd);
    add("password_enabled",        "0");
    add("password",                def.password);
    add("max_concurrent_per_ip",   std::to_string(def.max_conn_per_ip));
    add("max_conn_per_min_per_ip", std::to_string(def.max_conn_rate_per_min));
    add("max_total_conn",          std::to_string(def.max_total_conn));
    add("log_dir",                 def.log_dir);
    add("history_size",            std::to_string(def.history_size));
    add("max_msg_per_window",      std::to_string(def.max_msg_per_window));
    add("msg_window_secs",         std::to_string(def.msg_window_secs));
    add("keepalive_interval_secs", std::to_string(def.keepalive_interval_secs));
    add("keepalive_timeout_secs",  std::to_string(def.keepalive_timeout_secs));
    add("ip_block_duration_secs",  std::to_string(def.ip_block_duration_secs));
    add("allow_version_mismatch",  "0");
    add("banned_names",            "admin,mod");
    add("motd_color",              "");
}

ServerConfig ServerConfig::load_or_create(const std::string& path) {
    if(!file_exists_cfg(path)) {
        ServerConfig def;
        write_default(path, def);
        return def;
    }
    Ini existing = read_ini_file(path);
    append_missing_server_keys(path, existing);
    return from_ini(existing);
}

// ---------- ClientConfig ----------

// Generic key-value map serialized as  hp1|val1;hp2|val2  (using | as separator)
static void parse_kv_map(const std::string& s,
                         std::unordered_map<std::string,std::string>& out,
                         char sep = '#') {
    std::stringstream ss(s); std::string item;
    while(std::getline(ss, item, ';')) {
        if(item.empty()) continue;
        auto p = item.find(sep);
        if(p == std::string::npos) continue;
        std::string k = item.substr(0, p);
        std::string v = item.substr(p + 1);
        if(!k.empty()) out[k] = v;
    }
}

static std::string serialize_kv_map(const std::unordered_map<std::string,std::string>& m,
                                    char sep = '#') {
    std::string s; bool first = true;
    for(const auto& kv : m) {
        if(!first) s += ';';
        first = false;
        s += kv.first; s += sep; s += kv.second;
    }
    return s;
}

ClientConfig ClientConfig::from_ini(const Ini& ini) {
    ClientConfig c;
    c.last_host       = ini.get("last_host", "");
    c.last_port       = (uint16_t)ini.get_int("last_port", 24816);
    c.last_username   = ini.get("last_username", "");
    c.show_timestamps          = ini.get_bool("show_timestamps",          true);
    c.show_server_ip           = ini.get_bool("show_server_ip",           false);
    c.client_log_enabled       = ini.get_bool("client_log_enabled",       false);
    c.check_version_on_launch  = ini.get_bool("check_version_on_launch",  true);

    // usernames: "name#HEX;name2;..."
    {
        std::stringstream ss(ini.get("usernames", "")); std::string item;
        while(std::getline(ss, item, ';')) {
            if(item.empty()) continue;
            auto p = item.find('#');
            std::string name = item, hex = "";
            if(p != std::string::npos) { name = item.substr(0, p); hex = item.substr(p); }
            if(!name.empty()) c.usernames.push_back({name, hex});
        }
    }

    // servers: "host:port;..."
    {
        std::stringstream ss(ini.get("servers", "")); std::string item;
        while(std::getline(ss, item, ';')) {
            if(item.empty()) continue;
            std::string host; uint16_t port = 24816;
            if(parse_host_port(item, host, port, 24816))
                c.servers.push_back({host, port});
        }
    }

    parse_kv_map(ini.get("server_admin_keys",    ""), c.admin_keys,           '#');
    parse_kv_map(ini.get("server_passwords",     ""), c.server_passwords,     '#');
    parse_kv_map(ini.get("server_display_names", ""), c.server_display_names, '|');
    return c;
}

Ini ClientConfig::to_ini(const ClientConfig& c) {
    Ini ini;
    ini.set("last_host",     c.last_host);
    ini.set("last_port",     std::to_string(c.last_port));
    ini.set("last_username", c.last_username);
    ini.set("show_timestamps",          c.show_timestamps          ? "1" : "0");
    ini.set("show_server_ip",           c.show_server_ip           ? "1" : "0");
    ini.set("client_log_enabled",       c.client_log_enabled       ? "1" : "0");
    ini.set("check_version_on_launch",  c.check_version_on_launch  ? "1" : "0");

    {
        std::string u;
        for(size_t i = 0; i < c.usernames.size(); ++i) {
            if(i) u += ';';
            u += c.usernames[i].first;
            if(!c.usernames[i].second.empty()) u += c.usernames[i].second;
        }
        ini.set("usernames", u);
    }
    {
        std::string s;
        for(size_t i = 0; i < c.servers.size(); ++i) {
            if(i) s += ';';
            s += c.servers[i].first + ":" + std::to_string(c.servers[i].second);
        }
        ini.set("servers", s);
    }
    ini.set("server_admin_keys",    serialize_kv_map(c.admin_keys,           '#'));
    ini.set("server_passwords",     serialize_kv_map(c.server_passwords,     '#'));
    ini.set("server_display_names", serialize_kv_map(c.server_display_names, '|'));
    return ini;
}
