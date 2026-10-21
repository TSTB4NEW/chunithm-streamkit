// 点歌队列逻辑测试（不加载游戏）。
#include <cstdio>
#include <string>
#include "app.h"

static int fails = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}
static int Count(const std::string& s, const std::string& sub) {
    int n = 0;
    for (size_t p = 0; (p = s.find(sub, p)) != std::string::npos; p += sub.size()) ++n;
    return n;
}

int main() {
    csk::App& app = csk::App::Get();
    if (!app.db.LoadTsv("tests/songdb_test.tsv")) { printf("db load failed\n"); return 2; }
    FILE* af = fopen("tests/queue_test_alias.tsv", "wb");
    if (af) {
        fputs("1003\t创造\n", af);
        fclose(af);
    }
    app.db.LoadAliases("tests/queue_test_alias.tsv");
    app.ConfigureRequests(true, 3);

    Check(app.AddSongRequest("alice", 101, "grievous") == "added", "add #1");
    Check(app.AddSongRequest("bob", 102, "grievous lady master") == "added", "add same song, diff MASTER");
    Check(app.AddSongRequest("carol", 103, "Grievous Lady MASTER") == "duplicate", "duplicate same diff");
    Check(app.AddSongRequest("dave", 104, "zzzzzz") == "not_found", "not found");
    Check(app.AddSongRequest("eve", 105, "hikari") == "added", "add #2");

    std::string j = app.BuildRequestsJson();
    Check(j.find("\"enabled\":true") != std::string::npos, "json enabled");
    Check(j.find("Grievous Lady") != std::string::npos, "json title #1");
    Check(j.find("\"difficulty\":3") != std::string::npos, "json difficulty MASTER");
    Check(j.find("\"difficulty\":-1") != std::string::npos, "json difficulty unspecified");

    Check(app.AddSongRequest("frank", 106, "テスト") == "full", "queue full");

    app.OnSongStarted(1001);   // 标记第一条为正在游玩，不立即移除
    std::string j2 = app.BuildRequestsJson();
    Check(Count(j2, "\"song_id\":1001") == 2, "playing does not remove request");
    Check(j2.find("\"playing\":true") != std::string::npos, "playing flag set");
    app.OnSongEnded();
    std::string j3 = app.BuildRequestsJson();
    Check(Count(j3, "\"song_id\":1001") == 1, "played request removed on song end");
    Check(j3.find("\"difficulty\":3") != std::string::npos, "MASTER request kept");

    app.OnSongStarted(1001);
    app.OnSongEnded();
    Check(app.BuildRequestsJson().find("\"song_id\":1001") == std::string::npos,
          "second same-song request removed on end");

    app.ClearRequests();
    Check(app.BuildRequestsJson().find("\"items\":[]") != std::string::npos, "clear");

    // 观众取消自己的点歌
    app.AddSongRequest("u1", 201, "grievous");
    app.AddSongRequest("u2", 202, "hikari");
    Check(app.CancelRequestsByUser(201, "u1") == 1, "cancel own request");
    Check(app.BuildRequestsJson().find("\"song_id\":1001") == std::string::npos, "cancelled gone");
    Check(app.CancelRequestsByUser(201, "u1") == 0, "cancel with nothing");
    Check(app.OwnerRemoveRequest("1") == "removed", "owner remove by index");
    Check(app.BuildRequestsJson().find("\"items\":[]") != std::string::npos, "owner remove emptied");

    app.AddSongRequest("u3", 203, "grievous");
    Check(app.OwnerRemoveRequest("grievous") == "removed", "owner remove by title");

    // 前置难度：空格 / 紧挨 / 英文前缀
    app.ClearRequests();
    Check(app.AddSongRequest("u4", 204, "黑 grievous") == "added", "leading color + space");
    Check(app.BuildRequestsJson().find("\"difficulty\":4") != std::string::npos,
          "leading color sets ULTIMA");
    app.ClearRequests();
    Check(app.AddSongRequest("u5", 205, "黑grievous") == "added", "leading color adjacent");
    Check(app.BuildRequestsJson().find("\"difficulty\":4") != std::string::npos,
          "adjacent color sets ULTIMA");
    app.ClearRequests();
    Check(app.AddSongRequest("u6", 206, "MASTER grievous") == "added", "leading latin");
    Check(app.BuildRequestsJson().find("\"difficulty\":3") != std::string::npos,
          "leading latin sets MASTER");
    app.ClearRequests();
    Check(app.AddSongRequest("u7", 207, "紫苑") == "added", "title starting with color");
    Check(app.BuildRequestsJson().find("\"difficulty\":-1") != std::string::npos,
          "color-prefixed title not split");
    app.ClearRequests();
    Check(app.AddSongRequest("u8", 208, "紫创造") == "added",
          "adjacent color + CJK alias");
    Check(app.BuildRequestsJson().find("\"difficulty\":3") != std::string::npos,
          "adjacent CJK alias sets MASTER");
    app.ClearRequests();

    // 难度不存在
    Check(app.AddSongRequest("u4", 204, "hikari ultima") == "diff_missing",
          "missing difficulty rejected");
    app.ClearRequests();
    Check(app.AddSongRequest("u9", 209, "grievous we") == "diff_missing",
          "WE rejected when song has no WE chart");
    app.ClearRequests();

    // 多个相似结果 -> 进入选择状态
    Check(app.AddSongRequest("u5", 205, "same title") == "select", "ambiguous enters select");
    std::string st = app.BuildStateJson(false);
    Check(st.find("\"active\":true") != std::string::npos, "pending active in state json");
    Check(st.find("Artist A") != std::string::npos && st.find("Artist B") != std::string::npos,
          "both candidates present");
    Check(app.HandleSelection(999, 1) == false, "other user cannot choose");
    Check(app.HandleSelection(205, 2) == true, "requester chooses #2");
    Check(app.BuildStateJson(false).find("\"active\":false") != std::string::npos,
          "pending cleared after choice");
    std::string qj = app.BuildRequestsJson();
    Check(qj.find("\"song_id\":1004") != std::string::npos ||
              qj.find("\"song_id\":1005") != std::string::npos,
          "chosen candidate added");

    // 游玩中的点歌移到队首并标记 playing
    app.ClearRequests();
    app.AddSongRequest("u6", 206, "hikari");
    app.AddSongRequest("u7", 207, "grievous");
    app.OnSongStarted(1001);
    std::string jp = app.BuildRequestsJson();
    Check(jp.find("\"playing\":true") != std::string::npos, "playing flag set");
    Check(jp.find("\"song_id\":1001") < jp.find("\"song_id\":1002"),
          "playing request moved to front");
    app.OnSongEnded();

    app.ClearRequests();
    Check(app.AddSongRequest("frank", 106, "we are back") == "added",
          "we are back matches WE'RE BACK");
    Check(app.BuildRequestsJson().find("\"song_id\":1007") != std::string::npos,
          "WE contraction fuzzy match");
    app.ClearRequests();
    Check(app.AddSongRequest("frank", 107, "grevous lady") == "added",
          "typo fuzzy add");
    Check(app.BuildRequestsJson().find("\"song_id\":1001") != std::string::npos,
          "typo selects Grievous Lady");
    app.ClearRequests();
    Check(app.AddSongRequest("frank", 108, "same title") == "select",
          "web choose enters selection");
    Check(app.ChoosePendingCandidate(0), "web choose candidate");
    Check(app.BuildRequestsJson().find("\"title\":\"Same Title\"") != std::string::npos,
          "web choose adds candidate");
    app.ClearRequests();

    // 运行时开关
    app.AddDanmaku("viewer", "hello chat");
    app.SetRequestsEnabled(false);
    Check(!app.RequestsEnabled(), "runtime disable");
    Check(app.BuildChatJson().find("hello chat") != std::string::npos,
          "chat survives request disable");
    Check(app.AddSongRequest("off", 400, "grievous") == "disabled",
          "request rejected when disabled");
    app.SetRequestsEnabled(true);
    Check(app.RequestsEnabled(), "runtime enable");

    // 随机点歌
    app.ClearRequests();
    Check(app.AddRandomRequest("rand", 300, "") == "added", "random all");
    Check(app.BuildRequestsJson().find("\"difficulty\":-1") != std::string::npos,
          "random all does not force difficulty");
    app.ClearRequests();
    Check(app.AddRandomRequest("rand", 304, "VERSE") == "added", "random exact VERSE");
    Check(app.BuildRequestsJson().find("\"song_id\":1001") != std::string::npos,
          "VERSE does not match X-VERSE");
    app.ClearRequests();
    Check(app.AddRandomRequest("rand", 305, "X-VERSE") == "added", "random exact X-VERSE");
    Check(app.BuildRequestsJson().find("\"song_id\":1002") != std::string::npos,
          "X-VERSE does not match X-VERSE-X");
    app.ClearRequests();
    Check(app.AddRandomRequest("rand", 306, "X-VERSE-X") == "added",
          "random exact X-VERSE-X");
    Check(app.BuildRequestsJson().find("\"song_id\":1004") != std::string::npos,
          "X-VERSE-X matched");
    app.ClearRequests();
    Check(app.AddRandomRequest("rand", 308, "XVERSE") == "added",
          "XVERSE alias for X-VERSE");
    Check(app.BuildRequestsJson().find("\"song_id\":1002") != std::string::npos,
          "XVERSE matched X-VERSE");
    app.ClearRequests();
    Check(app.AddRandomRequest("rand", 309, "XVERSEX") == "added",
          "XVERSEX alias for X-VERSE-X");
    Check(app.BuildRequestsJson().find("\"song_id\":1004") != std::string::npos,
          "XVERSEX matched X-VERSE-X");
    app.ClearRequests();
    Check(app.AddRandomRequest("rand", 307, "MATE") == "added", "random MATE");
    Check(app.BuildRequestsJson().find("\"song_id\":1003") != std::string::npos,
          "MATE selects MATE song");
    app.ClearRequests();
    app.db.MergeSongMetaJson(
        "{\"songs\":[{\"id\":1001,\"genre\":\"东方Project\",\"version\":11000}]}");
    Check(app.AddRandomRequest("rand", 301, "东方") == "added", "random by genre");
    Check(app.BuildRequestsJson().find("\"song_id\":1001") != std::string::npos,
          "random genre picked matching song");
    app.ClearRequests();
    Check(app.AddRandomRequest("rand", 302, "AIR") == "added", "random by version");
    app.ClearRequests();
    Check(app.AddRandomRequest("rand", 303, "14.5") == "added", "random by level");
    Check(app.BuildRequestsJson().find("\"difficulty\":4") != std::string::npos,
          "random level picks chart difficulty");
    app.ClearRequests();

    // 持久化：写入 -> 清空 -> 手动放回文件 -> LoadQueue
    app.SetQueuePath("tests/queue_test.tsv");
    app.ClearRequests();
    FILE* qf = fopen("tests/queue_test.tsv", "wb");
    if (qf) {
        fputs("# csk-songrequest-v1\n", qf);
        fputs("7\t1002\t777\tHikari\t某艺术家\tu7\tHikari\t-1\t12345\n", qf);
        fclose(qf);
    }
    app.LoadQueue();
    std::string loaded = app.BuildRequestsJson();
    Check(loaded.find("\"song_id\":1002") != std::string::npos, "queue persisted and loaded");
    Check(loaded.find("\"id\":7") != std::string::npos, "request id restored");
    Check(loaded.find("\"playing\":false") != std::string::npos, "playing reset on load");

    app.ClearRequests();
    Check(app.BuildRequestsJson().find("\"items\":[]") != std::string::npos, "persist clear");

    for (int i = 0; i < 6; ++i) app.AddDanmaku("u", "msg" + std::to_string(i));
    std::string chat = app.BuildChatJson();
    Check(chat.find("msg0") == std::string::npos, "chat keeps last 5 (old dropped)");
    Check(chat.find("msg5") != std::string::npos, "chat newest present");

    printf("%s\n", fails ? "SOME FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
