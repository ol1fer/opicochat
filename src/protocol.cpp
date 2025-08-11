#include "protocol.hpp"
#include "utils.hpp"
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

// --- Welcome / history -------------------------------------------------------

std::string make_welcome(const std::string& server_name, bool pass_required){
    // Keep a stable, simple format: "WELCOME <name> <0|1>"
    return "WELCOME " + server_name + " " + (pass_required? "1":"0");
}

bool parse_welcome(const std::string& line, std::string& server_name, bool& pass_required){
    if(line.rfind("WELCOME ",0)!=0) return false;
    auto rest = line.substr(8);
    auto p = rest.rfind(' '); if(p == std::string::npos) return false; server_name = rest.substr(0, p); pass_required = (rest.substr(p+1) == "1");
    return true;
}

std::string make_req_history(int n){ return "REQHISTORY " + std::to_string(n); }

bool parse_history_header(const std::string& line, int& n){
    if(line.rfind("HISTORY ",0)!=0) return false;
    auto rest = line.substr(8);
    try{ n = std::stoi(rest); }catch(...){ return false; }
    return true;
}

// --- Auth (user, pass, optional color, optional admin key) -------------------

std::string make_auth(const std::string& user,
                      const std::string& pass,
                      const std::string& color_hex_opt,
                      const std::string& admin_key_opt)
{
    std::string s = "AUTH " + user;
    s += " " + (pass.empty()? "-" : pass);
    if(!color_hex_opt.empty()) s += " " + color_hex_opt; // "#RRGGBB" or "RRGGBB"
    if(!admin_key_opt.empty()) s += " " + admin_key_opt;
    return s;
}

bool parse_auth(const std::string& line,
                std::string& user,
                std::string& pass,
                std::string& color_hex_opt, bool& has_color,
                std::string& admin_key,    bool& has_key)
{
    user.clear(); pass.clear(); color_hex_opt.clear(); admin_key.clear();
    has_color=false; has_key=false;

    if(line.rfind("AUTH ",0)!=0) return false;
    auto rest = line.substr(5);
    auto v = split(rest, ' ');
    if(v.empty()) return false;

    user = v[0];
    if(v.size()>=2 && v[1]!="-") pass = v[1];
    if(v.size()>=4){ color_hex_opt=v[2]; has_color=true; admin_key=v[3]; has_key=true; }
else if(v.size()==3){
    if(is_valid_hex_color(v[2])){ color_hex_opt=v[2]; has_color=true; }
    else { admin_key=v[2]; has_key=true; }
}
    return true;
}

// --- Messages ----------------------------------------------------------------

std::string make_msg(const std::string& text){ return "MSG " + text; }
bool parse_msg(const std::string& line, std::string& text){
    if(line.rfind("MSG ",0)!=0) return false; text = line.substr(4); return true;
}

// --- Chat lines (iso, name, color_or_dash, text...) --------------------------

std::string make_chat(const std::string& iso,
                      const std::string& name,
                      const std::string& color_hex_or_dash,
                      const std::string& text)
{
    std::string color = color_hex_or_dash.empty()? "-" : color_hex_or_dash;
    return "CHAT " + iso + " " + name + " " + color + " " + text;
}

bool parse_chat(const std::string& line,
                std::string& iso,
                std::string& name,
                std::string& color_hex_or_dash,
                std::string& text)
{
    if(line.rfind("CHAT ",0)!=0) return false;
    auto rest = line.substr(5);
    auto v = split(rest, ' ', 4); // iso, name, color, text
    if(v.size()<4) return false;
    iso  = v[0];
    name = v[1];
    color_hex_or_dash = v[2];
    text = v[3];
    return true;
}

} // namespace proto
