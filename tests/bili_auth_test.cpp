// bili.h 登录逻辑离线测试：auth.txt 读写、Set-Cookie 解析、请求头、真实 B站 代理。
#include <windows.h>
#include <cstdio>
#include <string>
#include "bili.h"
#include "log.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::string ReadFile(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return "";
    std::string s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}
static void WriteFile(const char* p, const std::string& s) {
    FILE* f = fopen(p, "wb");
    if (f) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
}

int main() {
    csk::L().Init("tests\\bili_auth_test.log", 2);
    const char* authPath = "tests\\bili_auth_test.txt";
    DeleteFileA(authPath);
    WriteFile(authPath, "SESSDATA=faketoken123; bili_jct=deadbeef; DedeUserID=12345; DedeUserID__ckMd5=abc123");

    csk::bili::SetAuthPath(authPath);
    csk::bili::LoadAuth();
    Check(csk::bili::HasAuth(), "LoadAuth: has auth");
    Check(csk::bili::CurrentUid() == 12345, "LoadAuth: uid=12345");

    std::wstring hdr = csk::bili::detail::RequestHeaders();
    std::string h(hdr.begin(), hdr.end());
    Check(h.find("Cookie: SESSDATA=faketoken123") != std::string::npos, "RequestHeaders: cookie present");
    Check(h.find("User-Agent:") != std::string::npos, "RequestHeaders: UA present");

    unsigned gen0 = csk::bili::detail::AuthGen().load();
    std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Set-Cookie: buvid3=xyz789; Path=/; Domain=.bilibili.com\r\n"
        "Set-Cookie: SESSDATA=newtoken%2C999; Path=/; Domain=.bilibili.com; HttpOnly\r\n"
        "Set-Cookie: bili_jct=newjct; Path=/\r\n"
        "Set-Cookie: DedeUserID=67890; Path=/\r\n"
        "\r\n";
    csk::bili::detail::AbsorbSetCookie(raw);
    Check(csk::bili::CurrentUid() == 67890, "Absorb: uid switched to 67890");
    Check(csk::bili::detail::AuthGen().load() == gen0 + 1, "Absorb: auth generation bumped");
    std::string saved = ReadFile(authPath);
    Check(saved.find("SESSDATA=newtoken%2C999") != std::string::npos, "Absorb: SESSDATA persisted");
    Check(saved.find("DedeUserID=67890") != std::string::npos, "Absorb: DedeUserID persisted");
    Check(saved.find("buvid3=xyz789") != std::string::npos, "Absorb: extra cookie persisted");
    Check(saved.find("faketoken123") == std::string::npos, "Absorb: old SESSDATA replaced");

    std::string qr;
    bool ok = csk::bili::ProxyGet("passport.bilibili.com",
                                  "/x/passport-login/web/qrcode/generate", qr);
    printf("generate ok=%d resp=%.220s\n", ok ? 1 : 0, qr.c_str());
    Check(ok && qr.find("\"code\":0") != std::string::npos, "B站 qrcode/generate code=0");
    std::string key = csk::bili::detail::JStr(qr, "qrcode_key");
    Check(!key.empty(), "B站 qrcode_key parsed");

    if (!key.empty()) {
        std::string pth = "/x/passport-login/web/qrcode/poll?qrcode_key=" + key;
        std::string poll;
        bool pok = csk::bili::ProxyGet("passport.bilibili.com", pth.c_str(), poll);
        printf("poll ok=%d resp=%.220s\n", pok ? 1 : 0, poll.c_str());
        Check(pok && poll.find("\"code\":0") != std::string::npos &&
                  (poll.find("86101") != std::string::npos ||
                   poll.find("86090") != std::string::npos ||
                   poll.find("86038") != std::string::npos),
              "B站 poll reachable (waiting/scan/expired)");
        Check(csk::bili::CurrentUid() == 67890, "poll failure does not clobber login");
    }

    std::string nav;
    bool nok = csk::bili::ProxyGet("api.bilibili.com", "/x/web-interface/nav", nav);
    printf("nav ok=%d resp=%.220s\n", nok ? 1 : 0, nav.c_str());
    Check(nok && nav.find("\"code\":-101") != std::string::npos, "nav rejects fake cookie (-101)");

    printf("failures=%d\n", g_fail);
    return g_fail ? 1 : 0;
}


