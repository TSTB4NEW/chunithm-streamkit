// chunithm-streamkit — log.h
// 轻量线程安全文件日志。级别：0=关闭 1=信息 2=调试。
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>

namespace csk {

class Logger {
public:
    void Init(const std::string& path, int level) {
        {
            std::lock_guard<std::mutex> g(mtx_);
            level_ = level;
            path_ = path;
            if (fp_) fclose(fp_);
            fp_ = nullptr;
            if (!path.empty() && level > 0) {
                fp_ = fopen(path.c_str(), "a");
                if (fp_) setvbuf(fp_, nullptr, _IONBF, 0);
            }
        }
        // 注意：不能在持锁状态调用 Info/Log（std::mutex 不可重入）
        Info("---- chunithm-streamkit logger start ----");
    }

    void Log(int level, const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        std::lock_guard<std::mutex> g(mtx_);
        if (fp_ && level <= level_) {
            fprintf(fp_, "[%u] %s\n", (unsigned)GetTickCount(), buf);
        }
        // 日志只写本机文件，不再输出到调试器/控制台，避免密钥被旁观窗口看到
    }

    void Info(const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        Log(1, "%s", buf);
    }
    void Debug(const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        Log(2, "%s", buf);
    }

    static Logger& Get() { static Logger inst; return inst; }

private:
    Logger() = default;
    std::mutex mtx_;
    FILE* fp_ = nullptr;
    int level_ = 1;
    std::string path_;
};

inline Logger& L() { return Logger::Get(); }

} // namespace csk
