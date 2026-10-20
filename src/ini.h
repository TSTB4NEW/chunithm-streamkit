// chunithm-streamkit — ini.h
// 极简 INI 解析器（section/key=value，';' 与 '#' 注释）。足够承载插件配置。
#pragma once
#include <string>
#include <map>
#include <cstdio>
#include "util.h"

namespace csk {

class Ini {
public:
    using Section = std::map<std::string, std::string>;

    bool Load(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        char line[4096];
        std::string cur = "";
        while (fgets(line, sizeof(line), f)) {
            std::string s = trim(line);
            if (s.empty() || s[0] == ';' || s[0] == '#') continue;
            if (s[0] == '[') {
                size_t end = s.find(']');
                if (end != std::string::npos) cur = trim(s.substr(1, end - 1));
                continue;
            }
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(s.substr(0, eq));
            std::string v = trim(s.substr(eq + 1));
            // 去掉行内注释（仅在 ';' 前有空白时视为注释）
            size_t cm = v.find(" ;");
            if (cm != std::string::npos) v = trim(v.substr(0, cm));
            if (!cur.empty() && !k.empty()) sections_[cur][k] = v;
        }
        fclose(f);
        return true;
    }

    std::string Get(const std::string& sec, const std::string& key,
                    const std::string& def = "") const {
        auto si = sections_.find(sec);
        if (si == sections_.end()) return def;
        auto ki = si->second.find(key);
        return ki == si->second.end() ? def : ki->second;
    }

    bool Has(const std::string& sec, const std::string& key) const {
        auto si = sections_.find(sec);
        return si != sections_.end() && si->second.count(key) > 0;
    }

    const Section* GetSection(const std::string& sec) const {
        auto si = sections_.find(sec);
        return si == sections_.end() ? nullptr : &si->second;
    }

private:
    std::map<std::string, Section> sections_;
};

} // namespace csk
