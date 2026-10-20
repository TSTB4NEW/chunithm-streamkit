// 验证 ParseDanmaku：只认 DANMU_MSG 自己的 info，按 info[1] / info[2][1] 取值。
#include <cstdio>
#include <string>
#include "bili.h"

static bool Run(const char* name, const std::string& body, const char* wantUser,
                const char* wantText) {
    csk::bili::Event ev;
    bool ok = csk::bili::detail::ParseDanmaku(body, ev);
    bool pass = ok && ev.user == wantUser && ev.text == wantText && ev.uid == 123;
    printf("%s %s: ok=%d uid=%u user=<%s> text=<%s>\n", pass ? "PASS" : "FAIL", name,
           ok ? 1 : 0, ev.uid, ev.user.c_str(), ev.text.c_str());
    return pass;
}

int main() {
    bool all = true;

    // 1) 前面还有别的命令；DANMU_MSG 的 info[0] 是带 extra 的对象
    all &= Run("object-head",
               "{\"cmd\":\"ONLINE_RANK_V2\",\"data\":{\"info\":[{\"uid\":1,\"uname\":\"45e5f1af\","
               "\"msg\":\"info\"}]}}\n"
               "{\"cmd\":\"DANMU_MSG\",\"info\":[{\"extra\":\"45e5f1af\",\"uid\":1},"
               "\"点歌 测试\",[123,\"测试观众\",0,0,0,10000,1,\"\"],[12,0,640,400,0,1,\"\",0]]}",
               "测试观众", "点歌 测试");

    // 2) 标准结构：info[0] 是数字数组
    all &= Run("standard",
               "{\"cmd\":\"DANMU_MSG\",\"info\":[[0,1,25,16777215,0,0,0,\"\",0,0,0,\"\",\"\",\"\",0],"
               "\"点歌 测试2\",[123,\"测试观众\",0,0,0,10000,1,\"\"]]}",
               "测试观众", "点歌 测试2");

    // 3) cmd 在 info 之后
    all &= Run("cmd-after-info",
               "{\"info\":[[0,1,25],\"点歌 测试3\",[123,\"测试观众\"]],\"cmd\":\"DANMU_MSG\"}",
               "测试观众", "点歌 测试3");

    // 4) 带后缀的 cmd
    all &= Run("cmd-suffix",
               "{\"cmd\":\"DANMU_MSG:4:0:2:2:2:0\",\"info\":[[0,1,25],\"点歌 测试4\","
               "[123,\"测试观众\"]]}",
               "测试观众", "点歌 测试4");

    printf("%s\n", all ? "ALL PASS" : "SOME FAIL");
    return all ? 0 : 1;
}
