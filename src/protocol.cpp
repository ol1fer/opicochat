#include "protocol.hpp"
#include <sstream>
#include <vector>

namespace {
    static std::vector<std::string> split(const std::string& s, char sep, int maxparts=-1){
        std::vector<std::string> out; std::string cur; int parts=0;
        for(char c: s){
            if(c==sep && (maxparts<0 || parts<maxparts-1)){ out.push_back(cur); cur.clear(); parts++; }
            else cur.push_back(c);
        }
        out.push_back(cur);
        return out;
    }
}

namespace proto {

std::string make_welcome(const std::string& server_name, bool pass_required){
    return "WELCOME " + server_name + " " + (pass_required? "1":"0");
}
bool parse_welcome(const std::string& line, std::string& server_name, bool& pass_required){
    if(line.rfind("WELCOME ",0)!=0) return false;
    auto rest = line.substr(8);
    auto v = split(rest, ' ');
    if(v.size()<2) return false;
    server_name = v[0];
    pass_required = (v[1]=="1");
    return true;
}

std::string make_auth(const std::string& user, const std::string& pass, const std::string& color_hex_opt){
    std::string s = "AUTH " + user;
    s += " " + (pass.empty()? "-" : pass);
    if(!color_hex_opt.empty()) s += " " + color_hex_opt; // allow "#RRGGBB" or "RRGGBB"
    return s;
}
bool parse_auth(const std::string& line, std::string& user, std::string& pass, std::string& color_hex_opt, bool& has_color){
    user.clear(); pass.clear(); color_hex_opt.clear(); has_color=false;
    if(line.rfind("AUTH ",0)!=0) return false;
    auto rest = line.substr(5);
    auto v = split(rest, ' ');
    if(v.empty()) return false;
    user = v[0];
    if(v.size()>=2 && v[1]!="-") pass = v[1];
    if(v.size()>=3){ color_hex_opt = v[2]; has_color=true; }
    return true;
}

std::string make_msg(const std::string& text){ return "MSG " + text; }
bool parse_msg(const std::string& line, std::string& text){
    if(line.rfind("MSG ",0)!=0) return false; text = line.substr(4); return true;
}

// CHAT with color field (or "-" for none)
std::string make_chat(const std::string& iso, const std::string& name, const std::string& color_hex_or_dash, const std::string& text){
    std::string color = color_hex_or_dash.empty()? "-" : color_hex_or_dash;
    return "CHAT " + iso + " " + name + " " + color + " " + text;
}
bool parse_chat(const std::string& line, std::string& iso, std::string& name, std::string& color, std::string& text){
    if(line.rfind("CHAT ",0)!=0) return false;
    auto rest = line.substr(5);
    auto v = split(rest, ' ', 4); // iso, name, color, text
    if(v.size()==3){ // backward-compat: CHAT iso name text
        iso = v[0]; name = v[1]; color = "-"; text = v[2]; return true;
    }
    if(v.size()<4) return false;
    iso = v[0]; name = v[1]; color = v[2]; text = v[3];
    return true;
}

// legacy / compatibility (unused now)
std::string make_req_history(int n){ return "REQHISTORY " + std::to_string(n); }
bool parse_history_header(const std::string& line, int& n){
    if(line.rfind("HISTORY ",0)!=0) return false;
    try{ n = std::stoi(line.substr(8)); }catch(...){ return false; }
    return true;
}

} // namespace proto
