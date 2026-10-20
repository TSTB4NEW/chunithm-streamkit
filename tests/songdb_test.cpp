// SongDb 点歌匹配测试。
#include <cstdio>
#include <string>
#include "songdb.h"

static int fails = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}
static void Expect(const csk::SongDb& db, const char* q, unsigned wantId) {
    const csk::SongInfo* si = db.FindBest(q);
    unsigned got = si ? si->id : 0;
    bool ok = got == wantId;
    printf("%s FindBest(\"%s\") -> %u %s\n", ok ? "PASS" : "FAIL", q, got,
           si ? si->title.c_str() : "(null)");
    if (!ok) ++fails;
}

int main() {
    const char* path = "tests/songdb_test.tsv";
    FILE* f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return 2; }
    fputs("1001\tGrievous Lady\tTeam Grimoire\t174\t3\t6\t10.5\t13.7\t14.5\t\t\t原创\tVERSE\n", f);
    fputs("1002\tHikari\t某艺术家\t150\t2\t5\t9\t12\t\t\t\t原创\tX-VERSE\n", f);
    fputs("1003\tテスト曲\tテスト\t120\t1\t4\t8\t11\t\t\t\t原创\tMATE\n", f);
    fputs("1004\tSame Title\tArtist A\t130\t2\t5\t9\t12.5\t\t\t\t原创\tX-VERSE-X\n", f);
    fputs("1005\tSame Title\tArtist B\t140\t3\t6\t10\t13\t\t\n", f);
    fputs("1006\t紫苑\t某艺术家\t128\t2\t5\t9\t12\t13.5\t\t\t原创\tSUN PLUS\n", f);
    fputs("1007\tWE'RE BACK!!\tZekk\t185\t3\t7\t12.9\t14.6\t\t\t\t原创\tNEW PLUS\n", f);
    fputs("1008\tTru'nembra\t穴山大輔\t195\t5\t10\t14.3\t15.5\t\t\t\t原创\t\n", f);
    fclose(f);

    csk::SongDb db;
    if (!db.LoadTsv(path)) { printf("load failed\n"); return 2; }

    const char* aliasPath = "tests/songdb_test_alias.tsv";
    FILE* af = fopen(aliasPath, "wb");
    if (af) {
        fputs("1001\tgg\t格雷\tgrieved\n", af);
        fputs("1003\t大猩猩\n", af);
        fputs("1008\t提2\t蚊子拉小提琴\t蚊子\n", af);
        fclose(af);
    }
    db.LoadAliases(aliasPath);

    Expect(db, "grievous lady", 1001);   // 精确（忽略大小写/空格）
    Expect(db, "grievous", 1001);        // 前缀
    Expect(db, "テスト", 1003);          // 包含
    Expect(db, "Team Grimoire", 1001);   // 曲师
    Expect(db, "1002", 1002);            // 纯数字按 ID
    Expect(db, "不存在的歌", 0);          // 未找到
    Expect(db, "gg", 1001);              // 别名精确
    Expect(db, "格雷", 1001);            // 中文别名
    Expect(db, "大猩猩", 1003);          // 另一首别名
    Expect(db, "we are back", 1007);     // we're 展开为 we are
    Expect(db, "grevous lady", 1001);    // 拼写模糊
    Expect(db, "蚊子小提琴", 1008);       // 中文漏字模糊

    db.MergeAliasesJson(
        "{\"aliases\":[{\"song_id\":1001,\"aliases\":[\"gg2\",\"新别名\"]}]}");
    Expect(db, "gg2", 1001);             // 在线 JSON 合并
    Expect(db, "新别名", 1001);

    size_t meta = db.MergeSongMetaJson(
        "{\"songs\":[{\"id\":1001,\"genre\":\"东方Project\",\"version\":11000},"
        "{\"id\":9999,\"genre\":\"niconico\",\"version\":10000}]}");
    bool metaOk = false;
    for (const auto& si : db.SnapshotSongs())
        if (si.id == 1001 && si.genre == "东方Project" && si.version == "AIR") metaOk = true;
    Check(meta == 1, "song meta merged count");
    Check(metaOk, "song meta genre/version");

    printf("%s\n", fails ? "SOME FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
