// chunithm-streamkit — util.h
// 通用小工具：字符串处理、十六进制解析、计时。仅依赖 Win32/CRT，无第三方库。
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace csk {

inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= 0x20) ++b;
    while (e > b && (unsigned char)s[e - 1] <= 0x20) --e;
    return s.substr(b, e - b);
}

inline bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

inline bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

inline std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// 解析 "0x1A" / "1234" / 空串 → 0
inline uintptr_t parse_hex(const std::string& s) {
    std::string t = trim(s);
    if (t.empty()) return 0;
    return (uintptr_t)strtoull(t.c_str(), nullptr, 0);
}

inline uint64_t parse_u64(const std::string& s, uint64_t def = 0) {
    std::string t = trim(s);
    if (t.empty()) return def;
    return strtoull(t.c_str(), nullptr, 0);
}

inline int parse_i32(const std::string& s, int def = 0) {
    std::string t = trim(s);
    if (t.empty()) return def;
    return (int)strtol(t.c_str(), nullptr, 0);
}

inline double parse_f64(const std::string& s, double def = 0.0) {
    std::string t = trim(s);
    if (t.empty()) return def;
    return strtod(t.c_str(), nullptr);
}

inline uint32_t now_ms() { return (uint32_t)GetTickCount(); }

// 插件数据目录：<dllDir>\chunithm_streamkit（不存在则创建，含 logs 子目录）
inline std::string DataDirOf(const std::string& dllDir) {
    std::string d = dllDir + "\\chunithm_streamkit";
    CreateDirectoryA(d.c_str(), nullptr);
    CreateDirectoryA((d + "\\logs").c_str(), nullptr);
    return d;
}

inline std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// JSON 字符串转义（保留 UTF-8 原文，只转义必要字符）
inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}

} // namespace csk
