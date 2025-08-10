#include "config.hpp"
#include "utils.hpp"
#include <fstream>
#include <sstream>

static Ini parse_ini(std::istream& in){
    Ini ini; std::string line; while(std::getline(in,line)){
        line = trim(line); if(line.empty()||line[0]=='#'||line[0]==';'||line[0]=='[') continue;
        auto pos = line.find('='); if(pos==std::string::npos) continue; 
        std::string k=trim(line.substr(0,pos)); std::string v=trim(line.substr(pos+1)); ini.kv[k]=v;
    } return ini;
}

int Ini::get_int(const std::string& k, int def) const { try{ return std::stoi(get(k)); }catch(...){ return def; }}
bool Ini::get_bool(const std::string& k, bool def) const {
    auto v = get(k);
    if(v.empty()) return def; if(v=="1"||iequals(v,"true")||iequals(v,"yes")) return true; if(v=="0"||iequals(v,"false")||iequals(v,"no")) return false; return def;
}
std::string Ini::serialize() const{ std::ostringstream oss; for(auto& kvp:kv){ oss<<kvp.first<<"="<<kvp.second<<"\n"; } return oss.str(); }

ServerConfig ServerConfig::from_ini(const Ini& ini){
    ServerConfig c; 
    c.port = (uint16_t)ini.get_int("port", 24816);
    c.server_name = ini.get("server_name","[server]");
    c.password_enabled = ini.get_bool("password_enabled", false);
    c.password = ini.get("password", "");
    c.max_conn_per_ip = ini.get_int("max_conn_per_ip", 5);
    c.max_conn_per_sec_per_ip = ini.get_int("max_conn_per_sec_per_ip", 3);
    c.max_total_conn = ini.get_int("max_total_conn", 200);
    c.log_dir = ini.get("log_dir", "logs");
    return c;
}

ServerConfig ServerConfig::load_or_create(const std::string& path){
    std::ifstream f(path);
    if(!f.good()){
        ServerConfig def{}; write_default(path, def); return def;
    }
    Ini ini = parse_ini(f); return from_ini(ini);
}

void ServerConfig::write_default(const std::string& path, const ServerConfig& cfg){
    std::ofstream f(path);
    f << "# opico chat server configuration\n";
    f << "# generated on first run.\n\n";
    f << "port="<<cfg.port<<"\n";
    f << "server_name="<<cfg.server_name<<"\n";
    f << "password_enabled="<<(cfg.password_enabled?"true":"false")<<"\n";
    f << "password="<<cfg.password<<"\n";
    f << "max_conn_per_ip="<<cfg.max_conn_per_ip<<"\n";
    f << "max_conn_per_sec_per_ip="<<cfg.max_conn_per_sec_per_ip<<"\n";
    f << "max_total_conn="<<cfg.max_total_conn<<"\n";
    f << "log_dir="<<cfg.log_dir<<"\n";
}

ClientConfig ClientConfig::from_ini(const Ini& ini){
    ClientConfig c; 
    c.last_host = ini.get("last_host", "");
    c.last_port = (uint16_t)ini.get_int("last_port", 24816);
    c.last_username = ini.get("last_username", "");

    // usernames stored as name#HEX;name#HEX...
    auto ulist = ini.get("usernames", "");
    std::stringstream ss1(ulist); std::string item;
    while(std::getline(ss1,item,';')){
        if(item.empty()) continue; auto p=item.find('#');
        std::string name=item, hex="";
        if(p!=std::string::npos){ name=item.substr(0,p); hex=item.substr(p); }
        c.usernames.push_back({name, hex});
    }
    // servers stored as host:port;host:port
    auto slist = ini.get("servers","" );
    std::stringstream ss2(slist); while(std::getline(ss2,item,';')){
        if(item.empty()) continue; std::string host; uint16_t port=24816; if(parse_host_port(item, host, port, 24816)) c.servers.push_back({host,port});
    }
    return c;
}

Ini ClientConfig::to_ini(const ClientConfig& c){
    Ini ini; ini.set("last_host", c.last_host); ini.set("last_port", std::to_string(c.last_port)); ini.set("last_username", c.last_username);
    std::string u=""; for(size_t i=0;i<c.usernames.size();++i){ if(i) u.push_back(';'); u += c.usernames[i].first; if(!c.usernames[i].second.empty()) u += c.usernames[i].second; }
    ini.set("usernames", u);
    std::string s=""; for(size_t i=0;i<c.servers.size();++i){ if(i) s.push_back(';'); s += c.servers[i].first+":"+std::to_string(c.servers[i].second);} ini.set("servers", s);
    return ini;
}
