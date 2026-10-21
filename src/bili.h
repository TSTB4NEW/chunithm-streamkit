// chunithm-streamkit — bili.h
// B站直播弹幕客户端（游客模式，零额外依赖）
//
// 设计要点：
//   * 全部通过动态加载 winhttp.dll 调用，不修改构建脚本的链接参数
//   * 认证包声明 protover=1，服务器返回未压缩 JSON，省掉 brotli 解码
//   * 只关心 DANMU_MSG 的文本（点歌指令只需要这个）
//   * 由 ini [danmaku] enabled 控制是否启动；关闭时完全不产生线程和连接
//
// 使用：
//   csk::bili::Config cfg; cfg.roomId = 123456;
//   csk::bili::Start(cfg, [](const Event& e) { ... });
//   ...
//   csk::bili::Stop();
#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "log.h"
#include "md5.h"
#include "brotli_dec.h"

namespace csk {
namespace bili {

struct Config {
    unsigned roomId = 0;
    int protover = 3;      // 3 = Brotli（B站 当前只接受 3）
};

struct Event {
    unsigned uid = 0;
    std::string user;      // 昵称（游客模式下可能被 B站 打码）
    std::string text;      // 弹幕文本 ← 点歌指令就靠这个
};

using Callback = std::function<void(const Event&)>;

namespace detail {

// ---- winhttp.dll 动态入口 ----
struct Api {
    HMODULE mod = nullptr;
    decltype(&WinHttpOpen) pOpen = nullptr;
    decltype(&WinHttpConnect) pConnect = nullptr;
    decltype(&WinHttpOpenRequest) pOpenRequest = nullptr;
    decltype(&WinHttpSendRequest) pSendRequest = nullptr;
    decltype(&WinHttpReceiveResponse) pReceiveResponse = nullptr;
    decltype(&WinHttpReadData) pReadData = nullptr;
    decltype(&WinHttpQueryHeaders) pQueryHeaders = nullptr;
    decltype(&WinHttpCloseHandle) pClose = nullptr;
    decltype(&WinHttpSetOption) pSetOption = nullptr;
    decltype(&WinHttpSetTimeouts) pSetTimeouts = nullptr;
    decltype(&WinHttpWebSocketCompleteUpgrade) pWsUpgrade = nullptr;
    decltype(&WinHttpWebSocketSend) pWsSend = nullptr;
    decltype(&WinHttpWebSocketReceive) pWsRecv = nullptr;
    decltype(&WinHttpWebSocketClose) pWsClose = nullptr;

    bool Load() {
        static std::mutex loadMtx;
        std::lock_guard<std::mutex> g(loadMtx);
        if (mod) return true;
        mod = LoadLibraryA("winhttp.dll");
        if (!mod) return false;
        pOpen = (decltype(pOpen))GetProcAddress(mod, "WinHttpOpen");
        pConnect = (decltype(pConnect))GetProcAddress(mod, "WinHttpConnect");
        pOpenRequest = (decltype(pOpenRequest))GetProcAddress(mod, "WinHttpOpenRequest");
        pSendRequest = (decltype(pSendRequest))GetProcAddress(mod, "WinHttpSendRequest");
        pReceiveResponse = (decltype(pReceiveResponse))GetProcAddress(mod, "WinHttpReceiveResponse");
        pReadData = (decltype(pReadData))GetProcAddress(mod, "WinHttpReadData");
        pQueryHeaders = (decltype(pQueryHeaders))GetProcAddress(mod, "WinHttpQueryHeaders");
        pClose = (decltype(pClose))GetProcAddress(mod, "WinHttpCloseHandle");
        pSetOption = (decltype(pSetOption))GetProcAddress(mod, "WinHttpSetOption");
        pSetTimeouts = (decltype(pSetTimeouts))GetProcAddress(mod, "WinHttpSetTimeouts");
        pWsUpgrade = (decltype(pWsUpgrade))GetProcAddress(mod, "WinHttpWebSocketCompleteUpgrade");
        pWsSend = (decltype(pWsSend))GetProcAddress(mod, "WinHttpWebSocketSend");
        pWsRecv = (decltype(pWsRecv))GetProcAddress(mod, "WinHttpWebSocketReceive");
        pWsClose = (decltype(pWsClose))GetProcAddress(mod, "WinHttpWebSocketClose");
        return pOpen && pConnect && pOpenRequest && pSendRequest && pReceiveResponse &&
               pReadData && pQueryHeaders && pClose && pWsUpgrade && pWsSend && pWsRecv;
    }
};

// 从 JSON 里粗略取值（够用即可，不引入 JSON 库）
inline std::string JStr(const std::string& s, const char* key, size_t from = 0) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat, from);
    if (p == std::string::npos) return "";
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p < s.size() && s[p] == '"') {
        ++p;
        std::string out;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) {
                char c = s[p + 1];
                if (c == 'n') out += '\n';
                else if (c == 't') out += '\t';
                else if (c == 'u') { p += 5; continue; }   // 跳过 \uXXXX
                else out += c;
                p += 2;
            } else {
                out += s[p++];
            }
        }
        return out;
    }
    size_t e = p;
    while (e < s.size() && s[e] != ',' && s[e] != '}' && s[e] != ']') ++e;
    return s.substr(p, e - p);
}

inline unsigned JUint(const std::string& s, const char* key, size_t from = 0) {
    std::string v = JStr(s, key, from);
    unsigned n = 0;
    for (char c : v) {
        if (c < '0' || c > '9') break;
        n = n * 10 + (unsigned)(c - '0');
    }
    return n;
}

// ---- 运行时状态 ----
struct State {
    std::atomic<bool> running{false};
    std::thread th;
    Api api;
    wchar_t host[128]{};
    unsigned port = 443;
    std::string token;
    unsigned roomId = 0;
    int protover = 1;
    Callback cb;
    HINTERNET ws = nullptr;
};
inline State& S() {
    static State s;
    return s;
}

// ---- 协议小工具（B站 协议全部大端）----
inline void Put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)((x >> 24) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)(x & 0xFF));
}
inline void Put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)(x & 0xFF));
}

inline std::vector<uint8_t> MakePacket(uint32_t op, const std::string& body) {
    std::vector<uint8_t> p;
    Put32(p, (uint32_t)(16 + body.size()));
    Put16(p, 16);
    Put16(p, 1);
    Put32(p, op);
    Put32(p, 1);
    p.insert(p.end(), body.begin(), body.end());
    return p;
}

// ---- 极简 JSON 导航（只用于从 DANMU_MSG 里取字段）----
inline void JsonWs(const std::string& s, size_t& p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
}
inline bool JsonString(const std::string& s, size_t& p, std::string& out) {
    if (p >= s.size() || s[p] != '"') return false;
    ++p;
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) {
            char c = s[p + 1];
            if (c == 'n') out += '\n';
            else if (c == 't') out += '\t';
            else if (c == 'r') out += '\r';
            else if (c == 'b') out += '\b';
            else if (c == 'f') out += '\f';
            else if (c == 'u') { p += 6; continue; }   // 跳过 \uXXXX
            else out += c;
            p += 2;
        } else {
            out += s[p++];
        }
    }
    if (p < s.size() && s[p] == '"') { ++p; return true; }
    return false;
}
inline bool JsonSkipString(const std::string& s, size_t& p) {
    std::string dummy;
    return JsonString(s, p, dummy);
}
inline bool JsonUint(const std::string& s, size_t& p, unsigned& out) {
    JsonWs(s, p);
    size_t b = p;
    unsigned long long v = 0;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
        v = v * 10 + (unsigned)(s[p] - '0');
        if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull;
        ++p;
    }
    if (p == b) return false;
    out = (unsigned)v;
    return true;
}
inline bool JsonSkipValue(const std::string& s, size_t& p, int depth = 0) {
    JsonWs(s, p);
    if (p >= s.size() || depth > 64) return false;
    char c = s[p];
    if (c == '"') return JsonSkipString(s, p);
    if (c == '[') {
        ++p;
        for (;;) {
            JsonWs(s, p);
            if (p >= s.size()) return false;
            if (s[p] == ']') { ++p; return true; }
            if (!JsonSkipValue(s, p, depth + 1)) return false;
            JsonWs(s, p);
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == ']') { ++p; return true; }
            return false;
        }
    }
    if (c == '{') {
        ++p;
        for (;;) {
            JsonWs(s, p);
            if (p >= s.size()) return false;
            if (s[p] == '}') { ++p; return true; }
            if (!JsonSkipString(s, p)) return false;
            JsonWs(s, p);
            if (p >= s.size() || s[p] != ':') return false;
            ++p;
            if (!JsonSkipValue(s, p, depth + 1)) return false;
            JsonWs(s, p);
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == '}') { ++p; return true; }
            return false;
        }
    }
    while (p < s.size() && s[p] != ',' && s[p] != ']' && s[p] != '}' &&
           s[p] != ' ' && s[p] != '\r' && s[p] != '\n' && s[p] != '\t')
        ++p;
    return true;
}

// 从 DANMU_MSG 的 JSON 里取文本与用户名：info[1] / info[2][1]
inline bool ParseDanmaku(const std::string& j, Event& ev) {
    // 找到 cmd 字段的位置（兼容 "DANMU_MSG:4:0:..." 这类后缀）
    size_t cp = std::string::npos;
    for (size_t q = 0; (q = j.find("\"cmd\"", q)) != std::string::npos; q += 5) {
        size_t r = q + 5;
        JsonWs(j, r);
        if (r >= j.size() || j[r] != ':') continue;
        ++r;
        JsonWs(j, r);
        std::string cmd;
        if (JsonString(j, r, cmd) && cmd.rfind("DANMU_MSG", 0) == 0) { cp = q; break; }
    }
    if (cp == std::string::npos) return false;

    // info 可能在 cmd 前也可能在后，取距离最近的一个
    size_t after = j.find("\"info\":[", cp);
    size_t before = j.rfind("\"info\":[", cp);
    size_t ip = std::string::npos;
    if (after == std::string::npos) ip = before;
    else if (before == std::string::npos) ip = after;
    else ip = (after - cp <= cp - before) ? after : before;
    if (ip == std::string::npos) return false;

    size_t p = ip + 8;   // 跳过 "info":[
    JsonWs(j, p);
    if (!JsonSkipValue(j, p)) return false;              // info[0]
    JsonWs(j, p);
    if (p >= j.size() || j[p] != ',') return false;
    ++p;
    JsonWs(j, p);
    if (!JsonString(j, p, ev.text)) return false;        // info[1] = 文本
    JsonWs(j, p);
    if (p >= j.size() || j[p] != ',') return false;
    ++p;
    JsonWs(j, p);
    if (p >= j.size() || j[p] != '[') return false;      // info[2] = 用户数组
    ++p;
    JsonWs(j, p);
    unsigned uid = 0;
    if (!JsonUint(j, p, uid)) {
        if (!JsonSkipValue(j, p)) return false;          // uid（异常格式时跳过）
    }
    ev.uid = uid;
    JsonWs(j, p);
    if (p >= j.size() || j[p] != ',') return false;
    ++p;
    JsonWs(j, p);
    if (!JsonString(j, p, ev.user)) return false;        // uname
    return !ev.text.empty();
}

inline void SleepMs(int ms) { ::Sleep((DWORD)ms); }

} // namespace detail

// ---- 启动 / 停止 / 主循环 ----
inline bool Start(const Config& cfg, Callback cb);
inline void Stop();
inline bool Running();

// —— 扫码登录代理：本地页面调用，服务端代取 B站 接口（绕开 CORS）——
// host 用窄字符传入，内部转宽字符
inline bool ProxyGet(const char* host, const char* path, std::string& out);

namespace detail {

inline std::string Tidy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= 0x20) ++b;
    while (e > b && (unsigned char)s[e - 1] <= 0x20) --e;
    return s.substr(b, e - b);
}

inline std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// ---- 登录态：扫码得到的 Cookie 与 uid ----
// auth.txt 内容为一行 Cookie 头：SESSDATA=...; bili_jct=...; DedeUserID=...
inline std::mutex& AuthMutex() { static std::mutex m; return m; }
inline std::string& AuthPath() { static std::string p; return p; }
inline std::string& CookieJar() { static std::string s; return s; }
inline unsigned& AuthUid() { static unsigned u = 0; return u; }
inline std::atomic<unsigned>& AuthGen() { static std::atomic<unsigned> g{0}; return g; }

inline std::string CookieGet(const std::string& jar, const char* name) {
    std::string key = std::string(name) + "=";
    size_t p = 0;
    while ((p = jar.find(key, p)) != std::string::npos) {
        if (p == 0 || jar[p - 1] == ';' || jar[p - 1] == ' ') {
            size_t b = p + key.size();
            size_t e = jar.find(';', b);
            if (e == std::string::npos) e = jar.size();
            return Tidy(jar.substr(b, e - b));
        }
        p += key.size();
    }
    return "";
}

inline std::string MergeCookie(const std::string& base, const std::string& add) {
    std::map<std::string, std::string> m;
    auto load = [&m](const std::string& s) {
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find(';', p);
            if (e == std::string::npos) e = s.size();
            std::string item = Tidy(s.substr(p, e - p));
            size_t eq = item.find('=');
            if (eq != std::string::npos && eq > 0)
                m[Tidy(item.substr(0, eq))] = Tidy(item.substr(eq + 1));
            p = e + 1;
        }
    };
    load(base);
    load(add);
    std::string out;
    for (const auto& kv : m) {
        if (!out.empty()) out += "; ";
        out += kv.first;
        out += '=';
        out += kv.second;
    }
    return out;
}

// 调用处须持有 AuthMutex()
inline void SaveAuthLocked() {
    if (AuthPath().empty() || CookieJar().empty()) return;
    std::string tmp = AuthPath() + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    size_t n = fwrite(CookieJar().data(), 1, CookieJar().size(), f);
    fclose(f);
    if (n != CookieJar().size() ||
        !MoveFileExA(tmp.c_str(), AuthPath().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        return;
    }
    L().Info("auth: cookie saved (%zu bytes, uid=%u)", CookieJar().size(), AuthUid());
}

inline void LoadAuth() {
    std::lock_guard<std::mutex> g(AuthMutex());
    CookieJar().clear();
    AuthUid() = 0;
    if (AuthPath().empty()) return;
    FILE* f = fopen(AuthPath().c_str(), "rb");
    if (!f) return;
    std::string s;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    s = Tidy(s);
    if (s.empty()) return;
    CookieJar() = s;
    AuthUid() = (unsigned)strtoul(CookieGet(s, "DedeUserID").c_str(), nullptr, 10);
    L().Info("auth: loaded cookie (%zu bytes, uid=%u)", s.size(), AuthUid());
}

// 吸收响应头里的全部 Set-Cookie（B站 登录成功时会一次下发多条）
inline void AbsorbSetCookie(const std::string& rawHeaders) {
    if (rawHeaders.empty()) return;
    std::string incoming;
    size_t pos = 0;
    while (pos < rawHeaders.size()) {
        size_t eol = rawHeaders.find("\r\n", pos);
        if (eol == std::string::npos) eol = rawHeaders.size();
        std::string line = rawHeaders.substr(pos, eol - pos);
        pos = (eol == rawHeaders.size()) ? rawHeaders.size() : eol + 2;
        if (line.size() > 11 && _strnicmp(line.c_str(), "Set-Cookie:", 11) == 0) {
            std::string c = Tidy(line.substr(11));
            size_t semi = c.find(';');
            std::string nv = (semi == std::string::npos) ? c : c.substr(0, semi);
            if (nv.find('=') == std::string::npos) continue;
            if (!incoming.empty()) incoming += "; ";
            incoming += nv;
        }
    }
    if (incoming.empty()) return;
    bool hasSess = !CookieGet(incoming, "SESSDATA").empty();

    std::lock_guard<std::mutex> g(AuthMutex());
    std::string merged = MergeCookie(CookieJar(), incoming);
    bool changed = (merged != CookieJar());
    if (changed) {
        CookieJar() = merged;
        std::string uid = CookieGet(CookieJar(), "DedeUserID");
        if (!uid.empty()) AuthUid() = (unsigned)strtoul(uid.c_str(), nullptr, 10);
    }
    if (hasSess) {
        if (changed) SaveAuthLocked();
        AuthGen().fetch_add(1);
        L().Info("auth: login detected, danmaku will reconnect (uid=%u)", AuthUid());
    } else if (changed && AuthUid() != 0) {
        SaveAuthLocked();
    }
}

inline std::string CookieSnapshot() {
    std::lock_guard<std::mutex> g(AuthMutex());
    return CookieJar();
}

inline unsigned UidSnapshot() {
    std::lock_guard<std::mutex> g(AuthMutex());
    return AuthUid();
}

// 公共请求头：登录 Cookie（若有）+ 浏览器 UA
inline std::wstring RequestHeaders() {
    std::wstring h;
    std::string ck = CookieSnapshot();
    if (!ck.empty()) {
        h += L"Cookie: ";
        h += Widen(ck);
        h += L"\r\n";
    }
    h += L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         L"(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36\r\n";
    h += L"Accept: */*\r\n";
    return h;
}

// 新建 WinHTTP session，并关掉它自带的 Cookie 管理：
// 统一由 CookieJar / RequestHeaders 显式发送，避免和 Set-Cookie 自动回传重复。
inline HINTERNET OpenSession(Api& api) {
    HINTERNET ses = api.pOpen(L"chunithm-streamkit/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              L"", L"", 0);
    if (ses) {
        DWORD feat = WINHTTP_DISABLE_COOKIES;
        api.pSetOption(ses, WINHTTP_OPTION_DISABLE_FEATURE, &feat, sizeof(feat));
    }
    return ses;
}

inline std::string RawHeaders(Api& api, HINTERNET req) {
    DWORD sz = 0;
    api.pQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_NO_HEADER_INDEX, nullptr,
                      &sz, nullptr);
    if (sz < sizeof(wchar_t)) return "";
    std::vector<wchar_t> buf(sz / sizeof(wchar_t) + 1, 0);
    if (!api.pQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_NO_HEADER_INDEX,
                           buf.data(), &sz, nullptr))
        return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string out((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

// 打开 conn 并完成一次请求（HTTPS GET），返回响应体
inline bool Get(Api& api, HINTERNET ses, const wchar_t* host, const wchar_t* path,
                const wchar_t* referer, std::string& out) {
    HINTERNET con = api.pConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) return false;
    HINTERNET req = api.pOpenRequest(con, L"GET", path, nullptr, referer,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    bool ok = false;
    if (req) {
        std::wstring hdr = RequestHeaders();
        if (api.pSendRequest(req, hdr.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            api.pReceiveResponse(req, nullptr)) {
            DWORD code = 0, sz = sizeof(code);
            api.pQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_NO_HEADER_INDEX, &code, &sz, nullptr);
            AbsorbSetCookie(RawHeaders(api, req));
            if (code == 200) {
                DWORD got = 0;
                char buf[4096];
                while (api.pReadData(req, buf, sizeof(buf), &got) && got > 0) out.append(buf, got);
                ok = !out.empty();
            }
        }
        api.pClose(req);
    }
    api.pClose(con);
    return ok;
}

// 把「短房间号 / UID」解析成真实房间号（WS 认证必须用真实 room_id）
inline unsigned ResolveRoomId(Api& api, unsigned configured) {
    if (!configured) return 0;
    for (int i = 0; i < 2; ++i) {
        char path[256];
        if (i == 0)
            snprintf(path, sizeof(path), "/room/v1/Room/get_info?room_id=%u", configured);
        else
            snprintf(path, sizeof(path), "/room/v1/Room/getRoomInfoOld?mid=%u", configured);
        wchar_t wpath[256];
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 256);
        HINTERNET ses = OpenSession(api);
        if (!ses) break;
        std::string body;
        bool ok = Get(api, ses, L"api.live.bilibili.com", wpath,
                      L"https://live.bilibili.com/", body);
        api.pClose(ses);
        if (!ok) continue;
        unsigned rid = (i == 0) ? JUint(body, "room_id") : JUint(body, "roomid");
        if (rid) return rid;
    }
    return configured;
}

inline void MainLoop() {
    State& s = S();
    Api& api = s.api;
    if (!api.Load()) return;

    // 配置里可能是短房间号（= UID），WS 认证必须用真实 room_id
    unsigned realRoom = ResolveRoomId(api, s.roomId);
    if (realRoom && realRoom != s.roomId) {
        L().Info("dm: room %u resolved to %u", s.roomId, realRoom);
        s.roomId = realRoom;
    }

    while (s.running.load()) {
        // 1) 同一个 session 里：先访问首页拿 buvid3 等 cookie，再取弹幕服务器
        HINTERNET ses = OpenSession(api);
        std::string info;
        bool gotInfo = false;
        if (ses) {
            std::string warm;
            Get(api, ses, L"www.bilibili.com", L"/", L"https://www.bilibili.com/", warm);
            char path[384];
            char base[256];
            snprintf(base, sizeof(base), "id=%u&type=0&web_location=444.8", s.roomId);
            DWORD wts = (DWORD)time(nullptr);
            char q[320];
            snprintf(q, sizeof(q), "%s&wts=%u", base, (unsigned)wts);
            std::string sign = std::string(q) + "ea1db124af3c7062474693fa704f4ff8";
            std::string wrid = Md5Hex(sign);
            snprintf(path, sizeof(path),
                     "/xlive/web-room/v1/index/getDanmuInfo?%s&w_rid=%s", q, wrid.c_str());
            wchar_t wpath[256];
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 256);
            gotInfo = Get(api, ses, L"api.live.bilibili.com", wpath,
                          L"https://live.bilibili.com/", info);
            api.pClose(ses);
        }
        if (!gotInfo) { L().Info("dm: getDanmuInfo FAILED"); SleepMs(5000); continue; }
        L().Info("dm: getDanmuInfo ok");

        // 2) 解析 host_list[0] 与 token
        std::string token = JStr(info, "token");
        if (token.empty()) { L().Info("dm: token empty"); SleepMs(5000); continue; }
        size_t hp = info.find("\"host_list\":[");
        std::string hostStr;
        if (hp != std::string::npos) hostStr = JStr(info, "host", hp);
        if (hostStr.empty()) hostStr = "broadcastlv.chat.bilibili.com";
        wchar_t whost[128];
        MultiByteToWideChar(CP_UTF8, 0, hostStr.c_str(), -1, whost, 128);

        // 3) 建立 WebSocket
        HINTERNET ses2 = OpenSession(api);
        if (!ses2) { SleepMs(5000); continue; }
        if (api.pSetTimeouts) api.pSetTimeouts(ses2, 5000, 5000, 5000, 5000);
        HINTERNET con = api.pConnect(ses2, whost, 443, 0);
        HINTERNET req = con ? api.pOpenRequest(con, L"GET", L"/sub", nullptr,
                                               L"https://live.bilibili.com/",
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE)
                            : nullptr;
        bool upgraded = false;
        if (req) {
            api.pSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
            std::wstring hdr = RequestHeaders();
            if (api.pSendRequest(req, hdr.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA,
                                 0, 0, 0) &&
                api.pReceiveResponse(req, nullptr)) {
                s.ws = api.pWsUpgrade(req, 0);
                upgraded = (s.ws != nullptr);
            }
            if (!upgraded) api.pClose(req);
        }
        if (!upgraded && con) api.pClose(con);

        L().Info("dm: upgrade=%d host=%s", upgraded?1:0, hostStr.c_str());
        if (upgraded) {
            // 4) 认证（uid 取自扫码登录保存的 DedeUserID；0 = 游客）
            //    buvid 取自同一次会话里 www.bilibili.com 下发的 buvid3
            unsigned uid = UidSnapshot();
            std::string buvid;
            {
                std::lock_guard<std::mutex> g(AuthMutex());
                buvid = CookieGet(CookieJar(), "buvid3");
            }
            char auth[1024];
            snprintf(auth, sizeof(auth),
                     "{\"uid\":%u,\"roomid\":%u,\"protover\":%d,\"platform\":\"web\","
                     "\"type\":2,\"buvid\":\"%s\",\"key\":\"%s\"}",
                     uid, s.roomId, s.protover, buvid.c_str(), token.c_str());
            L().Info("dm: auth packet prepared (%zu bytes)", strlen(auth));
            auto pkt = MakePacket(7, auth);
            DWORD src2 = api.pWsSend(s.ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                     pkt.data(), (DWORD)pkt.size());
            L().Info("dm: auth rc=%lu len=%zu", (unsigned long)src2, pkt.size());

            unsigned authGen = AuthGen().load();
            DWORD lastBeat = GetTickCount();
            uint8_t buf[16384];
            while (s.running.load()) {
                if (AuthGen().load() != authGen) {
                    L().Info("dm: auth updated, reconnecting with new cookie");
                    break;
                }
                DWORD now = GetTickCount();
                if (now - lastBeat > 30000) {
                    lastBeat = now;
                    auto hb = MakePacket(2, "");
                    api.pWsSend(s.ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, hb.data(),
                                (DWORD)hb.size());
                }
                DWORD read = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
                DWORD rc = api.pWsRecv(s.ws, buf, sizeof(buf), &read, &type);
                if (rc != ERROR_SUCCESS || read == 0) {
                    if (rc == ERROR_WINHTTP_TIMEOUT) continue;
                    L().Info("dm: recv rc=%lu read=%lu", (unsigned long)rc, (unsigned long)read);
                    break;
                }
                // 解析包（可能一次含多个；全部大端）
                size_t off = 0;
                while (off + 16 <= read) {
                    uint32_t total = ((uint32_t)buf[off] << 24) |
                                     ((uint32_t)buf[off + 1] << 16) |
                                     ((uint32_t)buf[off + 2] << 8) | (uint32_t)buf[off + 3];
                    uint16_t hlen = ((uint16_t)buf[off + 4] << 8) | (uint16_t)buf[off + 5];
                    uint16_t ver = ((uint16_t)buf[off + 6] << 8) | (uint16_t)buf[off + 7];
                    uint32_t op = ((uint32_t)buf[off + 8] << 24) |
                                  ((uint32_t)buf[off + 9] << 16) |
                                  ((uint32_t)buf[off + 10] << 8) | (uint32_t)buf[off + 11];
                    if (hlen < 16) hlen = 16;
                    if (total < hlen || off + total > read) break;
                    if (op == 8) {   // 认证回应
                        std::string body((const char*)buf + off + hlen, total - hlen);
                        L().Info("dm: auth reply received");
                    } else if (op == 5) {
                        std::string body;
                        const uint8_t* payload = buf + off + hlen;
                        size_t plen = total - hlen;
                        if (ver == 3) {
                            std::string dec;
                            if (BrotliDecodeStr(payload, plen, dec)) body = dec;
                        } else if (ver == 0) {
                            body.assign((const char*)payload, plen);
                        }
                        if (!body.empty()) {
                            Event ev;
                            if (ParseDanmaku(body, ev) && s.cb) s.cb(ev);
                        }
                    }
                    off += total;
                }
            }
            if (s.ws) { api.pWsClose(s.ws, 1000, nullptr, 0); s.ws = nullptr; }
            L().Info("dm: loop exit");
        }
        if (con) api.pClose(con);
        if (ses2) api.pClose(ses2);

        SleepMs(3000);   // 断线重连
    }
}

} // namespace detail

// 通用 HTTPS GET（用于在线别名等只读接口；不读写 B 站 Cookie）
inline bool HttpGetText(const wchar_t* host, const wchar_t* path, std::string& out) {
    detail::Api& api = detail::S().api;
    if (!api.Load()) return false;
    HINTERNET ses = detail::OpenSession(api);
    if (!ses) return false;
    api.pSetTimeouts(ses, 5000, 5000, 8000, 8000);
    HINTERNET con = api.pConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) {
        api.pClose(ses);
        return false;
    }
    HINTERNET req = api.pOpenRequest(con, L"GET", path, nullptr, nullptr,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    bool ok = false;
    if (req) {
        std::wstring hdr = detail::RequestHeaders();
        if (api.pSendRequest(req, hdr.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            api.pReceiveResponse(req, nullptr)) {
            DWORD code = 0, sz = sizeof(code);
            api.pQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_NO_HEADER_INDEX, &code, &sz, nullptr);
            if (code == 200) {
                DWORD got = 0;
                char buf[4096];
                while (api.pReadData(req, buf, sizeof(buf), &got) && got > 0)
                    out.append(buf, got);
                ok = !out.empty();
            }
        }
        api.pClose(req);
    }
    api.pClose(con);
    api.pClose(ses);
    return ok;
}

// 发送直播弹幕（需要登录 Cookie 中的 bili_jct）
inline std::atomic<bool>& SendEnabled() {
    static std::atomic<bool> v{false};
    return v;
}
inline void SetSendEnabled(bool on) { SendEnabled().store(on); }

inline unsigned CurrentRoomId() { return detail::S().roomId; }

inline std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

inline bool HttpPostForm(const wchar_t* host, const wchar_t* path,
                         const std::string& body, const std::wstring& extra,
                         std::string& out) {
    detail::Api& api = detail::S().api;
    if (!api.Load()) return false;
    HINTERNET ses = detail::OpenSession(api);
    if (!ses) return false;
    api.pSetTimeouts(ses, 5000, 5000, 8000, 8000);
    HINTERNET con = api.pConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) { api.pClose(ses); return false; }
    HINTERNET req = api.pOpenRequest(con, L"POST", path, nullptr, nullptr,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    bool ok = false;
    if (req) {
        std::wstring hdr = detail::RequestHeaders();
        hdr += extra;
        char cl[64];
        snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n", body.size());
        hdr += L"Content-Type: application/x-www-form-urlencoded; charset=UTF-8\r\n";
        hdr += detail::Widen(std::string(cl));
        if (api.pSendRequest(req, hdr.c_str(), (DWORD)-1, (LPVOID)body.data(),
                             (DWORD)body.size(), (DWORD)body.size(), 0) &&
            api.pReceiveResponse(req, nullptr)) {
            DWORD code = 0, sz = sizeof(code);
            api.pQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_NO_HEADER_INDEX, &code, &sz, nullptr);
            if (code == 200) {
                DWORD got = 0;
                char buf[4096];
                while (api.pReadData(req, buf, sizeof(buf), &got) && got > 0)
                    out.append(buf, got);
                ok = !out.empty();
            }
        }
        api.pClose(req);
    }
    api.pClose(con);
    api.pClose(ses);
    return ok;
}

inline bool SendDanmaku(const std::string& msg, std::string& err) {
    if (!SendEnabled().load()) { err = "发送未开启"; return false; }
    unsigned room = CurrentRoomId();
    if (!room) { err = "弹幕未连接"; return false; }
    std::string cookie, jct;
    {
        std::lock_guard<std::mutex> g(detail::AuthMutex());
        cookie = detail::CookieJar();
        jct = detail::CookieGet(cookie, "bili_jct");
    }
    if (jct.empty()) { err = "缺少 bili_jct，请重新登录"; return false; }

    static std::mutex sendMtx;
    static uint64_t lastMs = 0;
    std::lock_guard<std::mutex> sg(sendMtx);
    uint64_t now = GetTickCount64();
    if (now - lastMs < 1600) { err = "发送太快，稍后再试"; return false; }
    lastMs = now;

    char rnd[32];
    snprintf(rnd, sizeof(rnd), "%llu", (unsigned long long)now);
    std::string body = "color=16777215&fontsize=25&mode=1&roomid=" +
                       std::to_string(room) + "&rnd=" + rnd +
                       "&msg=" + UrlEncode(msg) +
                       "&csrf=" + UrlEncode(jct) +
                       "&csrf_token=" + UrlEncode(jct);
    std::string out;
    std::wstring extra = L"Referer: https://live.bilibili.com/\r\n"
                         L"Origin: https://live.bilibili.com\r\n";
    if (!HttpPostForm(L"api.live.bilibili.com", L"/msg/send", body, extra, out)) {
        err = "网络请求失败";
        return false;
    }
    size_t p = out.find("\"code\"");
    int code = -1;
    if (p != std::string::npos) {
        size_t c = out.find(':', p);
        if (c != std::string::npos) code = atoi(out.c_str() + c + 1);
    }
    if (code != 0) {
        err = "B站拒绝（" + std::to_string(code) + "）";
        return false;
    }
    L().Info("chat send: ok len=%zu", msg.size());
    return true;
}

inline bool Start(const Config& cfg, Callback cb) {
    auto& s = detail::S();
    if (s.running.load()) return true;
    s.roomId = cfg.roomId;
    s.protover = cfg.protover;
    s.cb = std::move(cb);
    s.running.store(true);
    s.th = std::thread(&detail::MainLoop);
    return true;
}

inline void Stop() {
    auto& s = detail::S();
    s.running.store(false);
    if (s.th.joinable()) s.th.join();
}

inline bool Running() { return detail::S().running.load(); }

// —— 扫码登录：auth.txt 的路径 / 既存登录态 ——
inline void SetAuthPath(const std::string& p) { detail::AuthPath() = p; }
inline void LoadAuth() { detail::LoadAuth(); }
inline bool HasAuth() {
    std::lock_guard<std::mutex> g(detail::AuthMutex());
    return !detail::CookieGet(detail::CookieJar(), "SESSDATA").empty();
}
inline unsigned CurrentUid() { return detail::UidSnapshot(); }

inline bool ProxyGet(const char* host, const char* path, std::string& out) {
    auto& api = detail::S().api;
    if (!api.Load()) return false;
    wchar_t wh[128], wp[512];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, wh, 128);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 512);
    HINTERNET ses = detail::OpenSession(api);
    if (!ses) return false;
    bool ok = detail::Get(api, ses, wh, wp, L"https://www.bilibili.com/", out);
    api.pClose(ses);
    return ok;
}


} // namespace bili
} // namespace csk
