// chunithm-streamkit — httpd.cpp
#include "httpd.h"
#include "app.h"
#include "util.h"
#include "log.h"
#include "dds.h"
#include "bili.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <string>
#include <winhttp.h>

namespace csk {

// —— 封面缓存剪枝 ——
// 控制文件数量与总占用，超限时按最后写入时间删除最旧的
static const size_t kCacheMaxFiles = 256;
static const unsigned long long kCacheMaxBytes = 64ull * 1024 * 1024;

struct CacheEntry {
    std::string path;
    unsigned long long size;
    FILETIME write;
};

static void PruneJacketCache(const std::string& dir) {
    if (dir.empty()) return;
    std::vector<CacheEntry> items;
    unsigned long long total = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        CacheEntry e;
        e.path = dir + "\\" + fd.cFileName;
        e.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.write = fd.ftLastWriteTime;
        total += e.size;
        items.push_back(e);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (items.size() <= kCacheMaxFiles && total <= kCacheMaxBytes) return;

    std::sort(items.begin(), items.end(), [](const CacheEntry& a, const CacheEntry& b) {
        return CompareFileTime(&a.write, &b.write) < 0;   // 旧的在前
    });

    size_t count = items.size();
    size_t removed = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (count <= kCacheMaxFiles && total <= kCacheMaxBytes) break;
        if (DeleteFileA(items[i].path.c_str())) {
            total -= items[i].size;
            if (count > 0) --count;
            ++removed;
        }
    }
    if (removed) L().Info("jacket cache pruned: %zu files removed, %llu KB left",
                          removed, total / 1024);
}

static const char* kHdrJson =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json; charset=utf-8\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Cache-Control: no-store\r\n"
    "X-Content-Type-Options: nosniff\r\n";

static const char* kHdrHtml =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Cache-Control: no-store\r\n"
    "X-Content-Type-Options: nosniff\r\n";

static const char* kHdrSse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Cache-Control: no-store\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Connection: keep-alive\r\n\r\n";

static const char* kHdr404 =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 9\r\n"
    "Connection: close\r\n\r\nnot found";

static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string UrlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = HexVal(s[i + 1]), lo = HexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += (s[i] == '+') ? ' ' : s[i];
    }
    return out;
}

static std::string QueryParam(const std::string& path, const char* key) {
    size_t q = path.find('?');
    if (q == std::string::npos) return "";
    std::string query = path.substr(q + 1);
    std::string k = std::string(key) + "=";
    size_t p = 0;
    while ((p = query.find(k, p)) != std::string::npos) {
        if (p == 0 || query[p - 1] == '&') {
            size_t e = query.find('&', p);
            if (e == std::string::npos) e = query.size();
            return UrlDecode(query.substr(p + k.size(), e - p - k.size()));
        }
        p += k.size();
    }
    return "";
}

bool HttpServer::Start(uint16_t port, const std::string& host, const std::string& webRoot) {
    web_root_ = webRoot;
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
        L().Info("http: WSAStartup failed");
        return false;
    }
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET) {
        L().Info("http: socket() failed");
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr(host.c_str());
    a.sin_port = htons(port);
    if (a.sin_addr.s_addr == INADDR_NONE || a.sin_addr.s_addr == INADDR_ANY) {
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    if (bind(listen_, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        L().Info("http: bind %s:%u failed (err %d)", host.c_str(), port, WSAGetLastError());
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }
    if (listen(listen_, 8) == SOCKET_ERROR) {
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }
    running_ = true;
    accept_thread_ = std::thread(&HttpServer::AcceptLoop, this);
    L().Info("http server on http://%s:%u/", host.c_str(), port);
    return true;
}

void HttpServer::Stop() {
    if (!running_.exchange(false)) return;
    if (listen_ != INVALID_SOCKET) {
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
    }
    {
        std::lock_guard<std::mutex> g(clients_mtx_);
        for (SOCKET c : clients_) {
            shutdown(c, SD_BOTH);
            closesocket(c);
        }
        clients_.clear();
    }
    if (accept_thread_.joinable()) accept_thread_.join();
}

void HttpServer::AcceptLoop() {
    while (running_.load()) {
        SOCKET c = accept(listen_, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (!running_.load()) break;
            Sleep(100);
            continue;
        }
        std::thread(&HttpServer::ClientLoop, this, c).detach();
    }
}

void HttpServer::RegisterClient(SOCKET s) {
    std::lock_guard<std::mutex> g(clients_mtx_);
    clients_.push_back(s);
}
void HttpServer::UnregisterClient(SOCKET s) {
    std::lock_guard<std::mutex> g(clients_mtx_);
    for (size_t i = 0; i < clients_.size(); ++i) {
        if (clients_[i] == s) { clients_.erase(clients_.begin() + i); break; }
    }
}

void HttpServer::SendAll(SOCKET s, const char* b, size_t n) {
    size_t t = 0;
    while (t < n) {
        int k = send(s, b + t, (int)(n - t), 0);
        if (k <= 0) break;
        t += (size_t)k;
    }
}

bool HttpServer::ReadRequest(SOCKET s, std::string& out) {
    int timeoutMs = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    char buf[2048];
    int tries = 5;
    while (tries-- > 0 && running_.load()) {
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return false;
        buf[n] = 0;
        out += buf;
        if (out.size() > 16384) return false;
        if (out.find("\r\n\r\n") != std::string::npos) return true;
    }
    return false;
}

void HttpServer::ServeResponse(SOCKET s, const std::string& path) {
    App& app = App::Get();
    if (path == "/state" || path == "/state/") {
        std::string body = app.BuildStateJson(true);
        char cl[64];
        snprintf(cl, sizeof(cl), "Content-Length: %zu\r\nConnection: close\r\n\r\n", body.size());
        SendAll(s, kHdrJson, strlen(kHdrJson));
        SendAll(s, cl, strlen(cl));
        SendAll(s, body.data(), body.size());
        return;
    }
    if (path == "/health" || path == "/health/") {
        std::string body = app.BuildHealthJson();
        char cl[64];
        snprintf(cl, sizeof(cl), "Content-Length: %zu\r\nConnection: close\r\n\r\n", body.size());
        SendAll(s, kHdrJson, strlen(kHdrJson));
        SendAll(s, cl, strlen(cl));
        SendAll(s, body.data(), body.size());
        return;
    }
    if (path == "/debug" || path == "/debug/") {
        std::string body =
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>csk debug</title></head>"
            "<body style=\"background:#111;color:#eee;font-family:monospace\">"
            "<h3>chunithm-streamkit debug</h3><pre id=\"p\">waiting...</pre>"
            "<script>const es=new EventSource('/events');"
            "es.addEventListener('state',e=>{p.textContent=JSON.stringify(JSON.parse(e.data),null,2)})"
            "</script></body></html>";
        char cl[64];
        snprintf(cl, sizeof(cl), "Content-Length: %zu\r\nConnection: close\r\n\r\n", body.size());
        SendAll(s, kHdrHtml, strlen(kHdrHtml));
        SendAll(s, cl, strlen(cl));
        SendAll(s, body.data(), body.size());
        return;
    }

    // 自定义布局：读文件（没有则返回 {}）
    if (path == "/layout" || path == "/layout/") {
        std::string body;
        if (!layout_path_.empty()) {
            FILE* f = fopen(layout_path_.c_str(), "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0 && sz < (256 << 10)) {
                    body.resize((size_t)sz);
                    if (fread(&body[0], 1, body.size(), f) != body.size()) body.clear();
                }
                fclose(f);
            }
        }
        if (body.empty()) body = "{}";
        char cl[64];
        snprintf(cl, sizeof(cl), "Content-Length: %zu\r\nConnection: close\r\n\r\n", body.size());
        SendAll(s, kHdrJson, strlen(kHdrJson));
        SendAll(s, cl, strlen(cl));
        SendAll(s, body.data(), body.size());
        return;
    }

    // 封面：/jacket/<id>.png （id 可为 3 或 0003）
    if (path.rfind("/jacket/", 0) == 0) {
        std::string id = path.substr(8);
        size_t dot = id.find('.');
        if (dot != std::string::npos) id = id.substr(0, dot);
        ServeJacket(s, id);
        return;
    }

    // 点歌队列：GET /queue 返回 JSON；GET /queue/add?title=...&user=... 便于本地测试
    if (path.rfind("/queue", 0) == 0) {
        std::string body;
        if (path.rfind("/queue/add", 0) == 0) {
            std::string title = QueryParam(path, "title");
            if (title.empty()) title = QueryParam(path, "q");
            std::string user = QueryParam(path, "user");
            std::string st = app.AddSongRequest(user.empty() ? "manual" : user, 0, title);
            body = "{\"ok\":true,\"status\":\"" + st + "\"}";
        } else {
            body = "{\"ok\":true,\"request\":" + app.BuildRequestsJson() + "}";
        }
        std::string hdr = std::string(kHdrJson);
        char cl[80];
        snprintf(cl, sizeof(cl), "Content-Length: %zu\r\nConnection: close\r\n\r\n", body.size());
        hdr += cl;
        SendAll(s, hdr.c_str(), hdr.size());
        SendAll(s, body.c_str(), body.size());
        return;
    }

    // 登录态：/bili/auth 本地状态；/bili/me 代问 B站 nav（验证 cookie 是否有效）
    if (path.rfind("/bili/auth", 0) == 0) {
        std::string body = std::string("{\"logged_in\":") +
                           (bili::HasAuth() ? "true" : "false") +
                           ",\"uid\":" + std::to_string(bili::CurrentUid()) + "}";
        std::string hdr = std::string(kHdrJson);
        char cl[80];
        snprintf(cl, sizeof(cl), "Content-Length: %zu\r\nConnection: close\r\n\r\n", body.size());
        hdr += cl;
        SendAll(s, hdr.c_str(), hdr.size());
        SendAll(s, body.c_str(), body.size());
        return;
    }
    if (path.rfind("/bili/me", 0) == 0) {
        std::string body;
        if (!bili::ProxyGet("api.bilibili.com", "/x/web-interface/nav", body))
            body = "{\"code\":-1,\"message\":\"proxy failed\"}";
        std::string hdr = std::string(kHdrJson);
        char cl[80];
        snprintf(cl, sizeof(cl), "Content-Length: %zu\r\nConnection: close\r\n\r\n", body.size());
        hdr += cl;
        SendAll(s, hdr.c_str(), hdr.size());
        SendAll(s, body.c_str(), body.size());
        return;
    }

    // 扫码登录代理：/bili/qr         → 生成二维码
    //               /bili/qr/poll?key= → 轮询登录状态
    if (path.rfind("/bili/qr", 0) == 0) {
        std::string body;
        bool ok = false;
        if (path.rfind("/bili/qr/poll", 0) == 0) {
            size_t k = path.find("key=");
            if (k != std::string::npos) {
                std::string key;
                for (size_t i = k + 4; i < path.size(); ++i) {
                    char c = path[i];
                    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '_' || c == '-') key += c;
                    else break;
                }
                if (!key.empty()) {
                    std::string pth = "/x/passport-login/web/qrcode/poll?qrcode_key=" + key;
                    ok = bili::ProxyGet("passport.bilibili.com", pth.c_str(), body);
                }
            }
        } else {
            ok = bili::ProxyGet("passport.bilibili.com",
                                "/x/passport-login/web/qrcode/generate", body);
        }
        if (!ok) body = "{\"code\":-1,\"message\":\"proxy failed\"}";

        std::string hdr = std::string(kHdrJson);
        char cl[80];
        snprintf(cl, sizeof(cl), "Content-Length: %zu\r\nConnection: close\r\n\r\n", body.size());
        hdr += cl;
        SendAll(s, hdr.c_str(), hdr.size());
        SendAll(s, body.c_str(), body.size());
        return;
    }

    // 其余路径：仅允许 web 目录下单层安全文件名
    std::string name = path;
    // 去掉 query string（?code=... 之类）
    size_t qpos = name.find('?');
    if (qpos != std::string::npos) name = name.substr(0, qpos);
    if (name == "/" || name.empty()) name = "/index.html";
    if (name[0] == '/') name = name.substr(1);
    if (name.find('\\') != std::string::npos || name.find('/') != std::string::npos ||
        name.empty() || name.size() > 64) {
        SendAll(s, kHdr404, strlen(kHdr404));
        return;
    }
    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            SendAll(s, kHdr404, strlen(kHdr404));
            return;
        }
    }
    std::string full = web_root_ + "\\" + name;
    FILE* f = fopen(full.c_str(), "rb");
    if (!f) {
        SendAll(s, kHdr404, strlen(kHdr404));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (16 << 20)) { fclose(f); SendAll(s, kHdr404, strlen(kHdr404)); return; }
    std::string body;
    body.resize((size_t)sz);
    if (sz > 0) fread(&body[0], 1, (size_t)sz, f);
    fclose(f);
    const char* hdr = kHdrHtml;
    std::string ext;
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) ext = name.substr(dot);
    if (ext == ".css") {
        hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/css; charset=utf-8\r\n"
              "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n";
    } else if (ext == ".js") {
        hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/javascript; charset=utf-8\r\n"
              "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n";
    } else if (ext == ".png") {
        hdr = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n"
              "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n";
    } else if (ext == ".svg") {
        hdr = "HTTP/1.1 200 OK\r\nContent-Type: image/svg+xml\r\n"
              "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n";
    } else if (ext == ".json") {
        hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
              "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n";
    }
    char cl[64];
    snprintf(cl, sizeof(cl), "Content-Length: %zu\r\nConnection: close\r\n\r\n", body.size());
    SendAll(s, hdr, strlen(hdr));
    SendAll(s, cl, strlen(cl));
    SendAll(s, body.data(), body.size());
}

void HttpServer::ServeJacket(SOCKET s, const std::string& idStr) {
    // 归一化为 4 位（music0003）
    int id = 0;
    for (char c : idStr) {
        if (c < '0' || c > '9') break;
        id = id * 10 + (c - '0');
        if (id > 99999) break;
    }
    if (id <= 0) { SendAll(s, kHdr404, strlen(kHdr404)); return; }
    char id4[16];
    snprintf(id4, sizeof(id4), "%04d", id);
    std::string cachePath = cache_root_ + "\\" + id4 + ".png";

    std::vector<uint8_t> png;
    FILE* cf = fopen(cachePath.c_str(), "rb");
    if (cf) {
        fseek(cf, 0, SEEK_END);
        long sz = ftell(cf);
        fseek(cf, 0, SEEK_SET);
        if (sz > 0 && sz < (16 << 20)) {
            png.resize((size_t)sz);
            if (fread(png.data(), 1, png.size(), cf) != png.size()) png.clear();
        }
        fclose(cf);
    }
    if (png.empty()) {
        // 依次在各来源里找（更新包在前，基础包在后）
        std::string ddsPath;
        for (const std::string& root : jacket_roots_) {
            if (root.empty()) continue;
            char pat[MAX_PATH];
            snprintf(pat, sizeof(pat), "%s\\music%s\\CHU_UI_Jacket_*.dds", root.c_str(), id4);
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(pat, &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            ddsPath = root + "\\music" + id4 + "\\" + fd.cFileName;
            FindClose(h);
            break;
        }
        if (ddsPath.empty()) { SendAll(s, kHdr404, strlen(kHdr404)); return; }

        FILE* df = fopen(ddsPath.c_str(), "rb");
        if (!df) { SendAll(s, kHdr404, strlen(kHdr404)); return; }
        fseek(df, 0, SEEK_END);
        long sz = ftell(df);
        fseek(df, 0, SEEK_SET);
        if (sz <= 0 || sz > (16 << 20)) { fclose(df); SendAll(s, kHdr404, strlen(kHdr404)); return; }
        std::vector<uint8_t> raw((size_t)sz);
        if (fread(raw.data(), 1, raw.size(), df) != raw.size()) { fclose(df); SendAll(s, kHdr404, strlen(kHdr404)); return; }
        fclose(df);

        dds::Image img;
        if (!dds::Decode(raw.data(), raw.size(), img)) { SendAll(s, kHdr404, strlen(kHdr404)); return; }
        // 封面显示尺寸仅 142px，缩到 200px 存储可省一半以上体积
        dds::Downscale(img, 200);
        png = dds::EncodePng(img);

        if (!cache_root_.empty()) {
            CreateDirectoryA(cache_root_.c_str(), nullptr);
            FILE* wf = fopen(cachePath.c_str(), "wb");
            if (wf) { fwrite(png.data(), 1, png.size(), wf); fclose(wf); }
            PruneJacketCache(cache_root_);
        }
    }
    if (png.empty()) { SendAll(s, kHdr404, strlen(kHdr404)); return; }

    L().Info("jacket %s -> %zu bytes", id4, png.size());

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %zu\r\n"
             "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n"
             "Connection: close\r\n\r\n", png.size());
    SendAll(s, hdr, strlen(hdr));
    SendAll(s, (const char*)png.data(), png.size());
}

void HttpServer::ServeSse(SOCKET s) {
    RegisterClient(s);
    int sendTimeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeout, sizeof(sendTimeout));
    SendAll(s, kHdrSse, strlen(kHdrSse));

    App& app = App::Get();
    uint64_t clientSeq = 0;
    {
        std::lock_guard<std::mutex> g(app.mtx);
        clientSeq = app.snap.last_seq; // 只接收连接之后的增量事件
    }
    uint32_t lastState = 0;
    while (running_.load()) {
        // 1) 增量判定事件
        {
            std::vector<JudgeEvent> fresh;
            {
                std::lock_guard<std::mutex> g(app.mtx);
                for (const auto& ev : app.snap.feed) {
                    if (ev.seq > clientSeq) fresh.push_back(ev);
                }
            }
            for (const auto& ev : fresh) {
                std::string body = app.BuildEventJson(ev);
                std::string pkt = "id: " + std::to_string(ev.seq) + "\ndata: " + body + "\n\n";
                SendAll(s, pkt.data(), pkt.size());
                clientSeq = ev.seq;
            }
        }
        // 2) 全量状态（200ms 节流）
        uint32_t now = now_ms();
        if (now - lastState >= 200 || lastState == 0) {
            lastState = now;
            std::string body = app.BuildStateJson(false);
            std::string pkt = "data: " + body + "\n\n";
            if (send(s, pkt.data(), (int)pkt.size(), 0) <= 0) break;
        }
        Sleep(33);
    }
    UnregisterClient(s);
    closesocket(s);
}

void HttpServer::ClientLoop(SOCKET s) {
    std::string req;
    if (!ReadRequest(s, req)) {
        closesocket(s);
        return;
    }
    // 解析首行：GET /path HTTP/1.1
    size_t sp1 = req.find(' ');
    size_t sp2 = req.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        SendAll(s, kHdr404, strlen(kHdr404));
        closesocket(s);
        return;
    }
    std::string method = req.substr(0, sp1);
    std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);

    // 点歌队列操作：POST /queue/clear | /queue/remove?id=N | /queue/add?title=...
    if (method == "POST" && path.rfind("/queue/", 0) == 0) {
        App& app = App::Get();
        bool ok = false;
        if (path.rfind("/queue/clear", 0) == 0) {
            app.ClearRequests();
            ok = true;
        } else if (path.rfind("/queue/remove", 0) == 0) {
            uint64_t id = (uint64_t)strtoull(QueryParam(path, "id").c_str(), nullptr, 10);
            ok = app.RemoveRequest(id);
        } else if (path.rfind("/queue/add", 0) == 0) {
            std::string title = QueryParam(path, "title");
            if (title.empty()) title = QueryParam(path, "q");
            std::string user = QueryParam(path, "user");
            std::string st = app.AddSongRequest(user.empty() ? "manual" : user, 0, title);
            ok = (st == "added" || st == "duplicate");
        }
        const char* resp = ok
            ? "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
              "Access-Control-Allow-Origin: *\r\nContent-Length: 11\r\n"
              "Connection: close\r\n\r\n{\"ok\":true}"
            : "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
              "Access-Control-Allow-Origin: *\r\nContent-Length: 12\r\n"
              "Connection: close\r\n\r\n{\"ok\":false}";
        SendAll(s, resp, strlen(resp));
        closesocket(s);
        return;
    }

    // 运行时开关：POST /songrequest/enable | /disable | /toggle
    if (method == "POST" && path.rfind("/songrequest/", 0) == 0) {
        App& app = App::Get();
        bool enabled = app.RequestsEnabled();
        if (path.rfind("/songrequest/enable", 0) == 0) enabled = true;
        else if (path.rfind("/songrequest/disable", 0) == 0) enabled = false;
        else if (path.rfind("/songrequest/toggle", 0) == 0) enabled = !enabled;
        else {
            const char* bad = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                               "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n"
                               "{\"ok\":false}";
            SendAll(s, bad, strlen(bad));
            closesocket(s);
            return;
        }
        app.SetRequestsEnabled(enabled);
        std::string body = std::string("{\"ok\":true,\"enabled\":") +
                           (enabled ? "true" : "false") + "}";
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                           "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n" + body;
        SendAll(s, resp.c_str(), resp.size());
        closesocket(s);
        return;
    }

    // POST /dm：油猴脚本转发来的弹幕（纯文本 JSON: {"user":"..","text":".."}）
    if (method == "POST" && (path == "/dm" || path == "/dm/")) {
        size_t hdrEnd = req.find("\r\n\r\n");
        std::string body = (hdrEnd == std::string::npos) ? std::string() : req.substr(hdrEnd + 4);
        size_t clPos = req.find("Content-Length:");
        size_t need = 0;
        if (clPos != std::string::npos) need = (size_t)strtoul(req.c_str() + clPos + 15, nullptr, 10);
        while (body.size() < need && body.size() < (64 << 10)) {
            char b[2048];
            int n = recv(s, b, sizeof(b), 0);
            if (n <= 0) break;
            body.append(b, (size_t)n);
        }
        if (!body.empty()) {
            char line[1024];
            snprintf(line, sizeof(line), "dm: %.800s", body.c_str());
            L().Info("%s", line);
        }
        const char* resp =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\nContent-Length: 11\r\n"
            "Connection: close\r\n\r\n{\"ok\":true}";
        SendAll(s, resp, strlen(resp));
        closesocket(s);
        return;
    }

    // CORS 预检
    if (method == "OPTIONS") {
        const char* resp =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Access-Control-Max-Age: 86400\r\nConnection: close\r\n\r\n";
        SendAll(s, resp, strlen(resp));
        closesocket(s);
        return;
    }

    // POST /layout：保存自定义布局
    if (method == "POST" && (path == "/layout" || path == "/layout/")) {
        size_t hdrEnd = req.find("\r\n\r\n");
        std::string body = (hdrEnd == std::string::npos) ? std::string() : req.substr(hdrEnd + 4);
        size_t clPos = req.find("Content-Length:");
        size_t need = 0;
        if (clPos != std::string::npos) need = (size_t)strtoul(req.c_str() + clPos + 15, nullptr, 10);
        while (body.size() < need && body.size() < (256 << 10)) {
            char b[2048];
            int n = recv(s, b, sizeof(b), 0);
            if (n <= 0) break;
            body.append(b, (size_t)n);
        }
        bool ok = false;
        if (!layout_path_.empty() && !body.empty() && body.size() < (256 << 10)) {
            FILE* f = fopen(layout_path_.c_str(), "wb");
            if (f) {
                ok = (fwrite(body.data(), 1, body.size(), f) == body.size());
                fclose(f);
            }
        }
        const char* resp = ok
            ? "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
              "Access-Control-Allow-Origin: *\r\nContent-Length: 11\r\n"
              "Connection: close\r\n\r\n{\"ok\":true}"
            : "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n"
              "Access-Control-Allow-Origin: *\r\nContent-Length: 12\r\n"
              "Connection: close\r\n\r\n{\"ok\":false}";
        SendAll(s, resp, strlen(resp));
        closesocket(s);
        return;
    }

    if (method != "GET" && method != "HEAD") {
        SendAll(s, kHdr404, strlen(kHdr404));
        closesocket(s);
        return;
    }
    if (path == "/events" || path == "/events/") {
        ServeSse(s);
        return;
    }
    ServeResponse(s, path);
    closesocket(s);
}

} // namespace csk
