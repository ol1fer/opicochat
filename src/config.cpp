#include "config.hpp"
#include "utils.hpp"

#include <sstream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <unordered_map>

// --- tiny .ini file reader (key=value, ignores blank + lines starting with # or ;) ---
static Ini read_ini_file(const std::string& path){
    Ini ini;
    std::ifstream f(path);
    if(!f.good()) return ini;

    auto trim = [](const std::string& s){
        size_t a = 0; while(a < s.size() && std::isspace((unsigned char)s[a])) ++a;
        size_t b = s.size(); while(b > a && std::isspace((unsigned char)s[b-1])) --b;
        return s.substr(a, b - a);
    };

    std::string line;
    while(std::getline(f, line)){
        line = trim(line);
        if(line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto p = line.find('=');
        if(p == std::string::npos) continue;
        std::string k = trim(line.substr(0, p));
        std::string v = trim(line.substr(p + 1));
        if(!k.empty()) ini.set(k, v);
    }
    return ini;
}

// ---- Ini helpers implemented here ----
int Ini::get_int(const std::string& k, int def) const {
    auto it = kv.find(k); if(it == kv.end()) return def;
    try{ return std::stoi(it->second); } catch(...){ return def; }
}

bool Ini::get_bool(const std::string& k, bool def) const {
    auto it = kv.find(k); if(it == kv.end()) return def;
    std::string v = it->second;
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    if(v=="1" || v=="true"  || v=="yes" || v=="on")  return true;
    if(v=="0" || v=="false" || v=="no"  || v=="off") return false;
    return def;
}

std::string Ini::serialize() const {
    std::string s;
    for(const auto& kvp : kv){
        s += kvp.first; s += '='; s += kvp.second; s += '\n';
    }
    return s;
}

// -------- ServerConfig --------
ServerConfig ServerConfig::from_ini(const Ini& ini){
    ServerConfig c;
    c.port                    = (uint16_t)ini.get_int("port", 24816);
    c.server_name             = ini.get("server_name", "an opico chat server");
    c.motd                    = ini.get("motd", "");
    c.password_enabled        = ini.get_bool("password_enabled", false);
    c.password                = ini.get("password", "");
    c.max_conn_per_ip         = ini.get_int("max_concurrent_per_ip", ini.get_int("max_conn_per_ip", 8));
    c.max_conn_per_sec_per_ip = ini.get_int("max_conn_per_min_per_ip", ini.get_int("max_conn_per_sec_per_ip", 20));
    c.max_total_conn          = ini.get_int("max_total_conn", 200);
    c.log_dir                 = ini.get("log_dir", "logs");
    return c;
}

void ServerConfig::write_default(const std::string& path, const ServerConfig& cfg){
    std::ofstream f(path);
    f << "port="                     << cfg.port                      << "\n";
    f << "server_name="              << cfg.server_name               << "\n";
    f << "password_enabled="         << (cfg.password_enabled ? "1":"0") << "\n";
    f << "password="                 << cfg.password                  << "\n";
    f << "max_concurrent_per_ip="          << cfg.max_conn_per_ip           << "\n";
    f << "max_conn_per_min_per_ip="  << cfg.max_conn_per_sec_per_ip   << "\n";
    f << "max_total_conn="           << cfg.max_total_conn            << "\n";
    f << "log_dir="                  << cfg.log_dir                   << "\n";
    f << "motd="                     << cfg.motd                      << "\n";
}

ServerConfig ServerConfig::load_or_create(const std::string& path){
    std::ifstream f(path);
    if(!f.good()){
        ServerConfig def = from_ini(Ini{});
        write_default(path, def);
        return def;
    }
    Ini ini = read_ini_file(path);
    return from_ini(ini);
}

// -------- ClientConfig --------

// admin_keys helpers: "host:port#KEY;host2:port2#KEY2"
static void parse_admin_keys(const std::string& s,
                             std::unordered_map<std::string,std::string>& out){
    std::stringstream ss(s);
    std::string item;
    while(std::getline(ss, item, ';')){
        if(item.empty()) continue;
        auto p = item.find('#');
        if(p == std::string::npos) continue;
        std::string hp  = item.substr(0, p);
        std::string key = item.substr(p + 1);
        if(!hp.empty() && !key.empty()) out[hp] = key;
    }
                             }

                             static std::string serialize_admin_keys(const std::unordered_map<std::string,std::string>& m){
                                 std::string s; bool first = true;
                                 for(const auto& kv : m){
                                     if(!first) s.push_back(';'); first = false;
                                     s += kv.first; s.push_back('#'); s += kv.second;
                                 }
                                 return s;
                             }

                             ClientConfig ClientConfig::from_ini(const Ini& ini){
                                 ClientConfig c;
                                 c.last_host     = ini.get("last_host", "");
                                 c.last_port     = (uint16_t)ini.get_int("last_port", 24816);
                                 c.last_username = ini.get("last_username", "");

                                 // usernames: "name#HEX;name2#HEX2" (HEX may be empty)
                                 {
                                     auto ulist = ini.get("usernames", "");
                                     std::stringstream ss(ulist);
                                     std::string item;
                                     while(std::getline(ss, item, ';')){
                                         if(item.empty()) continue;
                                         auto p = item.find('#');
                                         std::string name = item, hex = "";
                                         if(p != std::string::npos){
                                             name = item.substr(0, p);
                                             hex  = item.substr(p); // keep leading '#'
                                         }
                                         c.usernames.push_back({name, hex});
                                     }
                                 }

                                 // servers: "host:port;host2:port2"
                                 {
                                     auto slist = ini.get("servers", "");
                                     std::stringstream ss(slist);
                                     std::string item;
                                     while(std::getline(ss, item, ';')){
                                         if(item.empty()) continue;
                                         std::string host; uint16_t port = 24816;
                                         if(parse_host_port(item, host, port, 24816)){
                                             c.servers.push_back({host, port});
                                         }
                                     }
                                 }

                                 // admin keys
                                 parse_admin_keys(ini.get("server_admin_keys",""), c.admin_keys);

                                 return c;
                             }

                             Ini ClientConfig::to_ini(const ClientConfig& c){
                                 Ini ini;
                                 ini.set("last_host", c.last_host);
                                 ini.set("last_port", std::to_string(c.last_port));
                                 ini.set("last_username", c.last_username);

                                 // usernames
                                 {
                                     std::string u;
                                     for(size_t i=0;i<c.usernames.size();++i){
                                         if(i) u.push_back(';');
                                         u += c.usernames[i].first;
                                         if(!c.usernames[i].second.empty()) u += c.usernames[i].second; // already includes '#'
                                     }
                                     ini.set("usernames", u);
                                 }

                                 // servers
                                 {
                                     std::string s;
                                     for(size_t i=0;i<c.servers.size();++i){
                                         if(i) s.push_back(';');
                                         s += c.servers[i].first + ":" + std::to_string(c.servers[i].second);
                                     }
                                     ini.set("servers", s);
                                 }

                                 // admin keys
                                 ini.set("server_admin_keys", serialize_admin_keys(c.admin_keys));

                                 return ini;
                             }
