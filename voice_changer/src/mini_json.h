// 极简 JSON: 解析扁平对象 {"k": "str" | 数字 | true/false | null}, 以及字符串转义 (界面通信用)
#pragma once
#include <cstdlib>
#include <map>
#include <string>

struct JVal {
    enum Type { Null, Num, Str, Bool } type = Null;
    double num = 0;
    std::string str;
    bool b = false;
};
using JObj = std::map<std::string, JVal>;

inline std::string json_escape(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o + "\"";
}

inline JObj json_parse_flat(const std::string& s) {
    JObj o;
    size_t i = 0, n = s.size();
    auto ws = [&] { while (i < n && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i; };
    auto str = [&]() {
        std::string r;
        ++i;  // "
        while (i < n && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < n) {
                char e = s[++i];
                if (e == 'n') r += '\n';
                else if (e == 't') r += '\t';
                else if (e == 'u' && i + 4 < n) {
                    unsigned cp = (unsigned)std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp < 0x80) r += (char)cp;
                    else if (cp < 0x800) { r += (char)(0xC0 | (cp >> 6)); r += (char)(0x80 | (cp & 0x3F)); }
                    else { r += (char)(0xE0 | (cp >> 12)); r += (char)(0x80 | ((cp >> 6) & 0x3F)); r += (char)(0x80 | (cp & 0x3F)); }
                } else r += e;
            } else r += s[i];
            ++i;
        }
        ++i;
        return r;
    };
    ws();
    if (i >= n || s[i] != '{') return o;
    ++i;
    for (;;) {
        ws();
        if (i >= n || s[i] == '}') break;
        if (s[i] != '"') break;
        std::string k = str();
        ws();
        if (i < n && s[i] == ':') ++i;
        ws();
        JVal v;
        if (i < n && s[i] == '"') { v.type = JVal::Str; v.str = str(); }
        else if (s.compare(i, 4, "true") == 0) { v.type = JVal::Bool; v.b = true; i += 4; }
        else if (s.compare(i, 5, "false") == 0) { v.type = JVal::Bool; v.b = false; i += 5; }
        else if (s.compare(i, 4, "null") == 0) { i += 4; }
        else { char* e = nullptr; v.type = JVal::Num; v.num = std::strtod(s.c_str() + i, &e); i = e - s.c_str(); }
        o[k] = v;
        ws();
        if (i < n && s[i] == ',') ++i;
    }
    return o;
}
