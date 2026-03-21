#include "protocol.hpp"
#include "utils.hpp"
#include <sstream>
#include <vector>
#include <cstdlib>

namespace {
std::vector<std::string> split_n(const std::string& s, char sep, int maxp) {
    std::vector<std::string> out;
    std::string cur;
    int parts = 0;
    for(char c : s) {
        if(c == sep && (maxp < 0 || parts < maxp-1)) { out.push_back(cur); cur.clear(); ++parts; }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}
} // anon

namespace proto {

std::string make_hello(const std::string& pub_hex) { return "HELLO " + pub_hex; }
bool parse_hello(const std::string& line, std::string& pub_hex) {
    if(line.rfind("HELLO ", 0) != 0) return false;
    pub_hex = line.substr(6);
    return pub_hex.size() == 64;
}

std::string make_pong(const std::string& version, const std::string& server_name,
                      int online, int max_conn, bool pass_req,
                      const std::string& name_color) {
    std::string s = "PONG " + version + "|" + server_name + "|"
        + std::to_string(online) + "/" + std::to_string(max_conn) + "|"
        + (pass_req ? "1" : "0");
    if(!name_color.empty()) s += "|" + name_color;
    return s;
}
bool parse_pong(const std::string& line, std::string& version, std::string& server_name,
                int& online, int& max_conn, bool& pass_req, std::string& name_color) {
    if(line.rfind("PONG ", 0) != 0) return false;
    auto v = split_n(line.substr(5), '|', 5);
    if(v.size() < 4) return false;
    version = v[0]; server_name = v[1]; pass_req = (v[3] == "1");
    name_color = (v.size() >= 5) ? v[4] : "";
    auto sl = split_n(v[2], '/', 2);
    if(sl.size() == 2) {
        try { online = std::stoi(sl[0]); max_conn = std::stoi(sl[1]); }
        catch(...) { online = 0; max_conn = 0; }
    }
    return true;
}

std::string make_welcome(const std::string& name, bool pass_req) {
    return "WELCOME " + name + " " + (pass_req ? "1" : "0");
}
bool parse_welcome(const std::string& line, std::string& name, bool& pass_req) {
    if(line.rfind("WELCOME ", 0) != 0) return false;
    auto v = split_n(line.substr(8), ' ', 2);
    if(v.size() < 2) return false;
    name = v[0]; pass_req = (v[1] == "1");
    return true;
}

std::string make_auth(const std::string& user, const std::string& pass,
                      const std::string& color, const std::string& key,
                      const std::string& version) {
    std::string s = "AUTH " + user + " " + (pass.empty() ? "-" : pass);
    if(!color.empty()) s += " " + color;
    if(!key.empty())   s += " " + key;
    if(!version.empty()) s += " v" + version;
    return s;
}
bool parse_auth(const std::string& line, std::string& user, std::string& pass,
                std::string& color, bool& has_color,
                std::string& key, bool& has_key, std::string& version) {
    if(line.rfind("AUTH ", 0) != 0) return false;
    auto v = split_n(line.substr(5), ' ', -1);
    if(v.empty()) return false;
    user = v[0];
    pass = (v.size() >= 2 && v[1] != "-") ? v[1] : "";
    has_color = false; has_key = false; version = "";
    for(size_t i = 2; i < v.size(); ++i) {
        if(!v[i].empty() && v[i][0] == 'v') { version = v[i].substr(1); }
        else if(is_valid_hex_color(v[i])) { color = v[i]; has_color = true; }
        else if(!v[i].empty()) { key = v[i]; has_key = true; }
    }
    return true;
}

std::string make_msg(const std::string& text) { return "MSG " + text; }
bool parse_msg(const std::string& line, std::string& text) {
    if(line.rfind("MSG ", 0) != 0) return false;
    text = line.substr(4); return true;
}

// CHAT / ACTION: <type> <iso> <name> <color|-> <role|-> <text...>
static std::string mk5(const std::string& type, const std::string& iso,
                       const std::string& name, const std::string& color,
                       const std::string& role, const std::string& text) {
    return type+" "+iso+" "+name+" "+(color.empty()?"-":color)+" "+(role.empty()?"-":role)+" "+text;
}
static bool p5(const std::string& type, const std::string& line,
               std::string& iso, std::string& name, std::string& color,
               std::string& role, std::string& text) {
    std::string pfx = type+" ";
    if(line.rfind(pfx,0)!=0) return false;
    auto v = split_n(line.substr(pfx.size()), ' ', 5);
    if(v.size()<5) return false;
    iso=v[0]; name=v[1]; color=v[2]; role=v[3]; text=v[4]; return true;
}

std::string make_chat(const std::string& iso, const std::string& n,
                      const std::string& c, const std::string& role, const std::string& t) {
    return mk5("CHAT",iso,n,c,role,t);
}
bool parse_chat(const std::string& l, std::string& i, std::string& n,
                std::string& c, std::string& role, std::string& t) {
    return p5("CHAT",l,i,n,c,role,t);
}

std::string make_action(const std::string& iso, const std::string& n,
                        const std::string& c, const std::string& role, const std::string& t) {
    return mk5("ACTION",iso,n,c,role,t);
}
bool parse_action(const std::string& l, std::string& i, std::string& n,
                  std::string& c, std::string& role, std::string& t) {
    return p5("ACTION",l,i,n,c,role,t);
}

// DM: DM <iso> <from> <color|-> <role|-> <to> <text...>
std::string make_dm(const std::string& iso, const std::string& from,
                    const std::string& color, const std::string& role,
                    const std::string& to, const std::string& text) {
    return "DM "+iso+" "+from+" "+(color.empty()?"-":color)+" "+(role.empty()?"-":role)+" "+to+" "+text;
}
bool parse_dm(const std::string& line, std::string& iso, std::string& from,
              std::string& color, std::string& role, std::string& to, std::string& text) {
    if(line.rfind("DM ",0)!=0) return false;
    auto v = split_n(line.substr(3), ' ', 6);
    if(v.size()<6) return false;
    iso=v[0]; from=v[1]; color=v[2]; role=v[3]; to=v[4]; text=v[5]; return true;
}

std::string make_rename(const std::string& a, const std::string& b) { return "RENAME "+a+" "+b; }
bool parse_rename(const std::string& l, std::string& a, std::string& b) {
    if(l.rfind("RENAME ",0)!=0) return false;
    auto v=split_n(l.substr(7),' ',2); if(v.size()<2) return false;
    a=v[0]; b=v[1]; return true;
}

std::string make_motd(const std::string& t, const std::string& color) {
    if(color.size() == 7 && color[0] == '#')
        return "MOTD " + color + " " + t;
    return "MOTD " + t;
}
bool parse_motd(const std::string& l, std::string& t, std::string& color) {
    if(l.rfind("MOTD ",0)!=0) return false;
    std::string body = l.substr(5);
    // optional leading "#rrggbb " color prefix
    if(body.size() >= 8 && body[0] == '#' && body[7] == ' ') {
        color = body.substr(0, 7);
        t     = body.substr(8);
    } else {
        color.clear();
        t = body;
    }
    return true;
}

std::string make_notice(const std::string& t) { return "NOTICE "+t; }
bool parse_notice(const std::string& l, std::string& t) {
    if(l.rfind("NOTICE ",0)!=0) return false;
    t=l.substr(7); return true;
}

std::string make_history_start(int n) { return "HISTORY_START "+std::to_string(n); }
std::string make_history_end() { return "HISTORY_END"; }

std::string make_admin_granted(const std::string& key) { return "ADMIN_GRANTED "+key; }
bool parse_admin_granted(const std::string& line, std::string& key) {
    if(line.rfind("ADMIN_GRANTED ",0)!=0) return false;
    key=line.substr(14); return !key.empty();
}

std::string make_mod_granted(const std::string& key) { return "MOD_GRANTED "+key; }
bool parse_mod_granted(const std::string& line, std::string& key) {
    if(line.rfind("MOD_GRANTED ",0)!=0) return false;
    key=line.substr(12); return !key.empty();
}

} // namespace proto
