// chunithm-streamkit 测试宿主（纯 C，无 C++ 运行时，模拟“注入到第三方进程”场景）
// 用法：test_host.exe <chunithm_streamkit.asi 路径> [端口]
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void http_get(const char* host, unsigned short port, const char* path,
                     int read_ms, int multi_frame) {
    WSADATA w;
    SOCKET s;
    struct sockaddr_in a;
    char buf[4096];
    char req[512];
    int timeout, frames = 0, got = 0;

    WSAStartup(MAKEWORD(2, 2), &w);
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { printf("[%s] socket failed\n", path); return; }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr(host);
    a.sin_port = htons(port);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        printf("[%s] connect failed\n", path);
        closesocket(s);
        return;
    }
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
    send(s, req, (int)strlen(req), 0);
    timeout = read_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    printf("===== %s =====\n", path);
    fflush(stdout);
    while (1) {
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
        got = 1;
        if (multi_frame) {
            int i;
            for (i = 0; i < n; ++i)
                if (buf[i] == '\n' && i > 0 && buf[i - 1] == '\n') ++frames;
            if (frames >= 2) break;
        } else {
            /* 头部收完后，短超时再收一轮，拿到 body */
            if (strstr(buf, "\r\n\r\n") != NULL) {
                int t = 300;
                int i;
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
                for (i = 0; i < 4; ++i) {
                    n = recv(s, buf, sizeof(buf) - 1, 0);
                    if (n <= 0) break;
                    buf[n] = 0;
                    fwrite(buf, 1, (size_t)n, stdout);
                    fflush(stdout);
                }
                break;
            }
        }
    }
    printf("\n===== /%s done (%s) =====\n", path, got ? "ok" : "empty");
    closesocket(s);
}

int main(int argc, char** argv) {
    HMODULE h;
    unsigned short port = 8770;
    void (*init)(void);

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        printf("usage: test_host.exe <path/to/chunithm_streamkit.asi> [port]\n");
        return 2;
    }
    if (argc > 2) port = (unsigned short)atoi(argv[2]);

    h = LoadLibraryA(argv[1]);
    if (!h) {
        printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    init = (void (*)(void))GetProcAddress(h, "InitializeASI");
    if (init) init();
    printf("ASI loaded, waiting for startup...\n");
    fflush(stdout);
    Sleep(1200);

    http_get("127.0.0.1", port, "/health", 2000, 0);
    http_get("127.0.0.1", port, "/state", 2000, 0);
    http_get("127.0.0.1", port, "/", 2000, 0);
    http_get("127.0.0.1", port, "/events", 3000, 1);

    /* 注意：不调用 FreeLibrary —— 与真实游戏一致，ASI 随进程存活。
       若卸载 DLL 而工作线程仍在运行，静态链接的 C++ 运行时会在卸载时
       触发 terminate（这是卸载竞争，不是游戏内场景）。 */
    printf("done\n");
    return 0;
}
