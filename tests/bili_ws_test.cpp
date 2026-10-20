// 不加载游戏，直接跑 bili 弹幕客户端：验证大端协议、房间解析、认证与弹幕解析。
// 用法：bili_ws_test.exe [auth.txt] [room_id] [seconds]
// 默认读取游戏目录的 auth.txt，房间 123456（会自动解析成真实 room_id）。
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include "bili.h"
#include "log.h"

int main(int argc, char** argv) {
    const char* auth =
        argc > 1 ? argv[1]
                 : "auth.txt";
    unsigned room = argc > 2 ? (unsigned)strtoul(argv[2], nullptr, 10) : 123456u;
    int secs = argc > 3 ? atoi(argv[3]) : 300;

    csk::L().Init("tests\\bili_ws_test.log", 2);
    csk::bili::SetAuthPath(auth);
    csk::bili::LoadAuth();

    csk::bili::Config cfg;
    cfg.roomId = room;
    cfg.protover = 3;
    csk::bili::Start(cfg, [](const csk::bili::Event& e) {
        printf("DANMU <%s> %s\n", e.user.c_str(), e.text.c_str());
        fflush(stdout);
        csk::L().Info("TEST danmaku <%s> %s", e.user.c_str(), e.text.c_str());
    });
    Sleep((DWORD)secs * 1000);
    csk::bili::Stop();
    return 0;
}
