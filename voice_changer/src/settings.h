// config.ini 读写 (key=value, UTF-8)
#pragma once
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

class Settings {
public:
    std::map<std::string, std::string> kv;
    std::filesystem::path path;

    void load(const std::filesystem::path& p) {
        path = p;
        std::ifstream f(p);
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            auto e = line.find('=');
            if (e == std::string::npos) continue;
            auto k = trim(line.substr(0, e)), v = trim(line.substr(e + 1));
            kv[k] = v;
        }
    }
    void save() const {
        std::ofstream f(path);
        f << "# 实时变声器配置 (程序退出时自动保存)\n";
        for (auto& [k, v] : kv) f << k << "=" << v << "\n";
    }
    std::string get(const std::string& k, const std::string& d = "") const {
        auto it = kv.find(k);
        return it == kv.end() ? d : it->second;
    }
    float getf(const std::string& k, float d) const { auto s = get(k); return s.empty() ? d : (float)std::atof(s.c_str()); }
    int geti(const std::string& k, int d) const { auto s = get(k); return s.empty() ? d : std::atoi(s.c_str()); }
    void set(const std::string& k, const std::string& v) { kv[k] = v; }
    void set(const std::string& k, double v) { kv[k] = std::to_string(v); }
    void set(const std::string& k, int v) { kv[k] = std::to_string(v); }

private:
    static std::string trim(std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        return s.substr(i);
    }
};
