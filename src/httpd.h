// chunithm-streamkit — httpd.h
// 只绑定回环地址的本机 HTTP 服务器，供 OBS 浏览器源读取。
// 端点：/ (覆盖层) /state (JSON 快照) /events (SSE 增量) /health /debug
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace csk {

class HttpServer {
public:
    bool Start(uint16_t port, const std::string& host, const std::string& webRoot);
    void Stop();
    bool running() const { return running_.load(); }

    // 布局文件：GET /layout 读取，POST /layout 保存
    void SetLayoutPath(const std::string& p) { layout_path_ = p; }

    // 封面：jacketRoots = 若干个 music 目录（按优先级排列），cacheDir = 插件 cache 目录
    void SetJacketRoots(const std::vector<std::string>& jacketRoots, const std::string& cacheDir) {
        jacket_roots_ = jacketRoots;
        cache_root_ = cacheDir;
    }

private:
    void AcceptLoop();
    void ClientLoop(SOCKET s);
    bool ReadRequest(SOCKET s, std::string& out);
    void SendAll(SOCKET s, const char* b, size_t n);
    void ServeResponse(SOCKET s, const std::string& path, const std::string& method);
    void ServeJacket(SOCKET s, const std::string& idStr);
    void ServeSse(SOCKET s);
    void RegisterClient(SOCKET s);
    void UnregisterClient(SOCKET s);

    SOCKET listen_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex clients_mtx_;
    std::vector<SOCKET> clients_;
    std::string web_root_;
    std::vector<std::string> jacket_roots_;
    std::string cache_root_;
    std::string layout_path_;
};

} // namespace csk
