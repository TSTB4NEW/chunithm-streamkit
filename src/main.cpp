// chunithm-streamkit — main.cpp
// ASI 插件入口：DllMain 启动工作线程；工作线程负责
//   加载配置 → 定位地址（只读）→ 周期采样 → 本机 HTTP/SSE。
// 全程不写游戏内存、不挂钩子、不改判定逻辑。
#include <windows.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <cstdlib>

#include "app.h"
#include "httpd.h"
#include "ini.h"
#include "log.h"
#include "memory.h"
#include "profile.h"
#include "bili.h"

#include "hook.h"
#include "sigscan.h"
#include "state.h"
#include "util.h"

namespace csk {

static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_started{false};
static HMODULE g_module = nullptr;
static HttpServer g_http;
static std::thread g_worker;
static HANDLE g_workerDone = nullptr;

static std::string DirOf(const std::string& path) {
    size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? std::string(".") : path.substr(0, p);
}

static std::string BaseName(const std::string& path) {
    size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? path : path.substr(p + 1);
}

static std::string DllDir() {
    char buf[MAX_PATH]{};
    if (g_module) GetModuleFileNameA(g_module, buf, sizeof(buf));
    return DirOf(buf);
}

// 启动早期跟踪（调试用）：在任何依赖初始化前直接写文件
static std::vector<std::string>& EarlyBootLines() {
    static std::vector<std::string> v;
    return v;
}

// 加载器锁内（DllMain）不做任何文件 I/O：capnhook 的 iohook 未初始化时
// 拦截文件 API 会断言。先把行缓冲在内存里，工作线程起来后再落盘。
static void BootTrace(const char* what) {
    char line[512];
    snprintf(line, sizeof(line), "[%u] %s", (unsigned)GetTickCount(), what);
    if (!csk::IoReady()) {
        EarlyBootLines().push_back(line);
        return;
    }
    char exeBuf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exeBuf, sizeof(exeBuf));
    std::string p = DataDirOf(DirOf(exeBuf)) + "\\logs\\csk_boot.log";
    FILE* f = fopen(p.c_str(), "a");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

static void FlushEarlyBoot() {
    if (EarlyBootLines().empty()) return;
    char exeBuf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exeBuf, sizeof(exeBuf));
    std::string p = DataDirOf(DirOf(exeBuf)) + "\\logs\\csk_boot.log";
    FILE* f = fopen(p.c_str(), "a");
    if (!f) return;
    for (auto& l : EarlyBootLines()) fprintf(f, "%s\n", l.c_str());
    fclose(f);
    EarlyBootLines().clear();
}

static void TerminateHandler() {
    char exeBuf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exeBuf, sizeof(exeBuf));
    std::string p = DataDirOf(DirOf(exeBuf)) + "\\logs\\csk_boot.log";
    FILE* f = fopen(p.c_str(), "a");
    if (f) {
        fprintf(f, "[%u] std::terminate called!\n", (unsigned)GetTickCount());
        fclose(f);
    }
    // 不重新抛异常，直接结束
    ExitProcess(0xDEAD);
}

// —— 演示模式：无需游戏即可验证 OBS 链路 ——
static void DemoThread() {
    App& app = App::Get();
    static const char* demoSongs[][3] = {
        {"928", "Grievous Lady", "Team Grimoire vs Laur"},
        {"2080", "HECATONCHEIR", "kors k"},
        {"15", "Climax", "USAO"},
    };
    int idx = 0;
    uint32_t songStart = now_ms();
    uint64_t jc = 0, ju = 0, at = 0, mi = 0;
    uint32_t score = 0;
    while (!g_stop.load()) {
        Sleep(250);
        uint32_t t = now_ms();
        if (t - songStart > 45000) { // 45s 换一首
            idx = (idx + 1) % 3;
            songStart = t;
            jc = ju = at = mi = 0;
            score = 0;
        }
        std::lock_guard<std::mutex> g(app.mtx);
        Snapshot& s = app.snap;
        s.demo = true;
        s.in_game = true;
        s.status = "demo";
        s.profile_ok = true;
        s.plugin_version = app.version;
        s.db_size = (uint32_t)app.db.size();
        s.profile_name = "demo";
        s.music_id = (uint32_t)strtoul(demoSongs[idx][0], nullptr, 10);
        s.difficulty = 3;
        int r = rand();
        if (r % 5 == 0) { ++mi; }
        else if (r % 7 == 0) { ++at; s.combo = 0; }
        else {
            if (r % 3 == 0) ++jc; else ++ju;
            s.combo = (s.combo + 1) % 10000;
            if (s.combo > s.max_combo) s.max_combo = s.combo;
            score += 1010;
            if (score > 1009000) score = 1009000;
        }
        s.jc = jc; s.ju = ju; s.at = at; s.mi = mi;
        s.score = score;
        s.note_total = s.max_combo;
        JudgeEvent ev;
        ev.seq = ++s.last_seq;
        ev.t_ms = t;
        ev.type = (r % 5 == 0) ? JUDGE_MI : (r % 7 == 0) ? JUDGE_AT : (r % 3 == 0) ? JUDGE_JC : JUDGE_JU;
        ev.lane = (uint8_t)(r % 16);
        ev.delta_ms = (int32_t)((r % 24) - 12);
        s.feed.push_back(ev);
        while (s.feed.size() > 256) s.feed.pop_front();
        // 歌曲名（演示用内置数据）
        SongInfo si;
        si.id = s.music_id;
        si.title = demoSongs[idx][1];
        si.artist = demoSongs[idx][2];
        si.bpm = 174.0;
        si.diff_const[3] = 13.7;
        si.known = true;
        app.demoSongs[s.music_id] = si;
    }
}

static void WorkerThread() {
    // 工作线程：先等游戏主流程跑起来，避开 capnhook iohook 的初始化窗口，
    // 期间不做任何文件 I/O（ini 读取也在等待之后）。
    Sleep(1500);
    csk::IoReady() = true;
    FlushEarlyBoot();
    csk::hook::FlushEarlyLog();
    BootTrace("worker start");
    App& app = App::Get();
    app.version = "0.1.0";
    std::string dllDir = DllDir();
    std::string dataDir = DataDirOf(dllDir);   // <bin>\chunithm_streamkit
    BootTrace(("dllDir=" + dllDir).c_str());

    // —— 配置（优先 data 目录，兼容旧版放在 bin 下）——
    Ini ini;
    std::string iniPath = dataDir + "\\chunithm_streamkit.ini";
    if (!ini.Load(iniPath)) {
        iniPath = dllDir + "\\chunithm_streamkit.ini";
        if (!ini.Load(iniPath)) iniPath.clear();
    }
    int logLevel = parse_i32(ini.Get("general", "log_level", "1"));
    std::string logFile = ini.Get("general", "log_file", "chunithm_streamkit.log");
    if (logFile.find(':') == std::string::npos && logFile.find('\\') == std::string::npos)
        logFile = dataDir + "\\logs\\" + logFile;
    L().Init(logFile, logLevel);
    L().Info("chunithm-streamkit %s starting (ini=%s)", app.version.c_str(),
             iniPath.empty() ? "(none)" : iniPath.c_str());
    app.config_path = iniPath;

    if (ini.Get("general", "enabled", "1") == "0") {
        L().Info("plugin disabled by [general] enabled=0");
        return;
    }

    int pollMs = parse_i32(ini.Get("general", "poll_ms", "16"), 16);
    if (pollMs < 4) pollMs = 4;
    std::string profileName = ini.Get("general", "profile", "mate-2.50");
    app.snap.profile_name = profileName;

    // —— 歌曲库 ——
    app.snap.db_size = (uint32_t)app.db.size();
    std::string dbHint;
    std::string tsv = dataDir + "\\music_db.tsv";
    if (!app.db.LoadTsv(tsv)) {
        if (tsv != (dllDir + "\\music_db.tsv") && app.db.LoadTsv(dllDir + "\\music_db.tsv")) {
            tsv = dllDir + "\\music_db.tsv";   // 兼容旧位置
        }
    }
    if (app.db.size() == 0) {
        char exeBuf[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exeBuf, sizeof(exeBuf));
        std::string gameRoot = DirOf(exeBuf);
        std::string xmlSrc = app.db.TryImportXml(gameRoot);
        if (!xmlSrc.empty()) {
            app.db.SaveTsv(tsv);
            L().Info("auto-imported song db to %s", tsv.c_str());
        } else {
            L().Info("no music_db.tsv and no importable music.xml found; "
                     "song titles will be unavailable (raw id only)");
            dbHint = "未找到 music_db.tsv 或 music.xml：请运行 tools/build_music_db.py 生成歌曲库，"
                     "或把 music_db.tsv 放到插件数据目录";
        }
    }
    app.snap.db_size = (uint32_t)app.db.size();
    if (app.db.size() > 0) {
        std::string aliasPath = dataDir + "\\music_alias.tsv";
        if (!app.db.LoadAliases(aliasPath) && aliasPath != (dllDir + "\\music_alias.tsv"))
            app.db.LoadAliases(dllDir + "\\music_alias.tsv");

        // 在线别名：启动后拉取落雪列表，并在长时间开播时每 6 小时刷新
        App* aliasApp = &app;
        std::thread([aliasApp] {
            for (;;) {
                std::string body;
                // 在线接口只允许补充别名；标题、曲师、定数、分类、版本一律以本地 music_db.tsv 为准
        if (bili::HttpGetText(L"maimai.lxns.net",
                                      L"/api/v0/chunithm/alias/list", body)) {
                    size_t added = aliasApp->db.MergeAliasesJson(body);
                    L().Info("online aliases merged: +%zu, total %zu", added,
                             aliasApp->db.AliasCount());
                } else {
                    L().Info("online alias fetch failed; using local music_alias.tsv");
                }
                Sleep(6 * 60 * 60 * 1000);
            }
        }).detach();
    }

    // —— 定位器 ——
    std::string moduleName = ini.Get("general", "module", "");
    if (moduleName.empty()) {
        char exeBuf[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exeBuf, sizeof(exeBuf));
        moduleName = BaseName(exeBuf);
    }
    ModuleScanner scanner;
    if (!scanner.Init(moduleName)) {
        L().Info("warning: module '%s' not loaded yet; base-relative offsets unavailable",
                 moduleName.c_str());
    }
    Profile profile;
    profile.Load(ini, profileName);

    // —— 分数/连击写入指令 inline hook（只捕获，不改变游戏逻辑）——
    // 是否安装由配置决定，方便出问题时一键关闭回退到「只读判定」
    bool wantScoreHook = parse_i32(ini.Get(profileName, "score.hook", "1"), 1) != 0;
    bool wantComboHook = parse_i32(ini.Get(profileName, "combo.hook", "0"), 0) != 0 ||
                         parse_i32(ini.Get(profileName, "max_combo.hook", "0"), 0) != 0;
    bool wantSongHook = parse_i32(ini.Get(profileName, "music_id.hook", "0"), 0) != 0;
    bool wantDiffHook = parse_i32(ini.Get(profileName, "difficulty.hook", "0"), 0) != 0;
    bool hookOk = wantScoreHook ? hook::Install() : false;
    bool comboHookOk = wantComboHook ? hook::InstallCombo() : false;
    bool songHookOk = wantSongHook ? hook::InstallSong() : false;
    bool diffHookOk = wantDiffHook ? hook::InstallDiff() : false;
    bool wantMaxScoreHook = parse_i32(ini.Get(profileName, "max_score.hook", "1"), 1) != 0;
    bool maxScoreHookOk = wantMaxScoreHook ? hook::InstallMaxScore() : false;
    bool wantBestScoreHook = parse_i32(ini.Get(profileName, "best_score.hook", "1"), 1) != 0;
    bool bestScoreHookOk = wantBestScoreHook ? hook::InstallBestScore() : false;
    // FAST/LATE 计数 hook：捕获统计数组元素地址，用于定位各计数器偏移
    bool flHookOk = hook::InstallFastLate();
    if (!flHookOk) {
        L().Info("FAST/LATE hook FAILED — early/late stats will stay 0. "
                 "Game update may have changed bytes at RVA 0x7B2E1D; "
                 "see logs\\csk_hook.log for details.");
    }
    L().Info("score hook: %s", hookOk ? "installed" : (wantScoreHook ? "failed" : "disabled"));
    L().Info("combo hook: %s", comboHookOk ? "installed" : (wantComboHook ? "failed" : "disabled"));
    L().Info("song hook: %s", songHookOk ? "installed" : (wantSongHook ? "failed" : "disabled"));
    L().Info("diff hook: %s", diffHookOk ? "installed" : (wantDiffHook ? "failed" : "disabled"));
    L().Info("max_score hook: %s", maxScoreHookOk ? "installed" : (wantMaxScoreHook ? "failed" : "disabled"));
    L().Info("best_score hook: %s", bestScoreHookOk ? "installed" : (wantBestScoreHook ? "failed" : "disabled"));

    // —— HTTP 先启动：内存扫描可能耗时，先让 OBS 能连上 ——
    uint16_t port = (uint16_t)parse_i32(ini.Get("server", "port", "8770"), 8770);
    std::string host = ini.Get("server", "host", "127.0.0.1");
    std::string webRoot = ini.Get("server", "web_root", "");
    // 相对路径按插件数据目录解析（兼容旧版 web_root=web）
    if (webRoot.empty() ||
        (webRoot.find(':') == std::string::npos && webRoot.find('\\') == std::string::npos)) {
        std::string sub = webRoot.empty() ? "web" : webRoot;
        webRoot = dataDir + "\\" + sub;
        if (GetFileAttributesA((webRoot + "\\index.html").c_str()) == INVALID_FILE_ATTRIBUTES)
            webRoot = dllDir + "\\" + sub;   // 旧版布局兜底
    }
    L().Info("web root: %s", webRoot.c_str());
    g_http.Start(port, host, webRoot);

    // 封面：DDS 分散在 bin\option\A*（版本更新包）和 data\A000（基础包）
    {
        std::string gameRoot = dllDir;
        size_t pos = gameRoot.find_last_of("\\/");
        if (pos != std::string::npos) gameRoot = gameRoot.substr(0, pos);

        std::vector<std::string> packs;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dllDir + "\\option\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == '.') continue;
                packs.push_back(fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        // 版本号大的优先（更新的包覆盖旧的）
        std::sort(packs.begin(), packs.end(),
                  [](const std::string& a, const std::string& b) { return a > b; });

        std::vector<std::string> roots;
        for (const std::string& p : packs)
            roots.push_back(dllDir + "\\option\\" + p + "\\music");
        roots.push_back(gameRoot + "\\data\\A000\\music");

        // 每次启动清空封面缓存，避免长期累积；运行期还有数量/容量剪枝兜底
        std::string cacheDir = dataDir + "\\cache";
        {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((cacheDir + "\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                size_t removed = 0;
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    if (DeleteFileA((cacheDir + "\\" + fd.cFileName).c_str())) ++removed;
                } while (FindNextFileA(h, &fd));
                FindClose(h);
                if (removed) L().Info("jacket cache cleared on start: %zu files", removed);
            }
        }

        g_http.SetJacketRoots(roots, cacheDir);
        g_http.SetLayoutPath(dataDir + "\\layout.json");
        bili::SetAuthPath(dataDir + "\\auth.txt");
        bili::LoadAuth();
        L().Info("jacket roots: %zu (packs=%zu)", roots.size(), packs.size());
    }

    // —— 地址定位（只读；找不到时周期性重扫，重扫次数受控）——
    profile.Resolve(scanner);
    app.snap.profile_ok = profile.AnyOk();

    // —— 演示模式 ——
    bool demo = parse_i32(ini.Get("general", "demo", "0")) != 0;
    bool wantDiffScan = parse_i32(ini.Get(profileName, "difficulty.scan", "0"), 0) != 0;
    std::thread demoThread;
    if (demo) demoThread = std::thread(&DemoThread);

    // —— 点歌模块（默认关闭，ini [songrequest] enabled 控制）——
    {
        bool reqOn = atoi(ini.Get("songrequest", "enabled", "0").c_str()) != 0;
        int reqMax = parse_i32(ini.Get("songrequest", "max", "12"), 12);
        app.ConfigureRequests(reqOn, reqMax);
        app.SetQueuePath(dataDir + "\\songrequest.tsv");
        if (reqOn) app.LoadQueue();
        unsigned ownerCfg =
            (unsigned)strtoul(ini.Get("songrequest", "owner_uid", "").c_str(), nullptr, 10);
        unsigned ownerUid = ownerCfg ? ownerCfg : bili::CurrentUid();
        app.owner_uid = ownerUid;
        if (reqOn)
            L().Info("songrequest: enabled (max=%d, owner_uid=%u, %s)", reqMax, ownerUid,
                     ownerCfg ? "config" : "login");

        bool dmuOn = atoi(ini.Get("danmaku", "enabled", "0").c_str()) != 0;
        bili::SetSendEnabled(atoi(ini.Get("danmaku", "allow_send", "0").c_str()) != 0);
        unsigned rid = (unsigned)atoi(ini.Get("danmaku", "room_id", "").c_str());
        if (dmuOn && rid == 0) {
            L().Info("danmaku: enabled but room_id empty, skipped");
        } else if (dmuOn) {
            bili::Config bc;
            bc.roomId = rid;
            bc.protover = atoi(ini.Get("danmaku", "protover", "1").c_str());
            App* ap = &app;
            auto dmCb = [ap](const bili::Event& e) {
                L().Info("danmaku <%s> %s", e.user.c_str(), e.text.c_str());
                std::string t = trim(e.text);

                // 房主可运行时开关点歌（关闭后仍允许再开启）
                static const char* kOff[] = {"关闭点歌", "关点歌", "点歌关闭", "停止点歌"};
                static const char* kOn[] = {"开启点歌", "开点歌", "点歌开启", "恢复点歌"};
                bool isOff = false, isOn = false;
                for (const char* c : kOff) if (t == c) isOff = true;
                for (const char* c : kOn) if (t == c) isOn = true;
                if (isOff || isOn) {
                    if (ap->owner_uid != 0 && e.uid == ap->owner_uid) {
                        ap->SetRequestsEnabled(isOn);
                    } else {
                        ap->SetNotice("denied", "只有房主可以开关点歌");
                    }
                    return;
                }

                if (!ap->request_enabled.load()) return;
                ap->AddDanmaku(e.user, e.text);

                // 待选择状态：点歌人回复 1-4
                if (t.size() == 1 && t[0] >= '1' && t[0] <= '9') {
                    if (ap->HandleSelection(e.uid, t[0] - '0')) return;
                }

                // 观众取消自己点的歌
                static const char* kCancel[] = {"取消点歌", "撤销点歌", "取消歌曲"};
                for (const char* c : kCancel) {
                    if (t == c) {
                        ap->CancelRequestsByUser(e.uid, e.user);
                        return;
                    }
                }

                // 只有房主可以清空 / 删歌
                static const char* kClear[] = {"清空点歌", "清空歌曲", "清空队列"};
                for (const char* c : kClear) {
                    if (t != c) continue;
                    if (ap->owner_uid != 0 && e.uid == ap->owner_uid) {
                        ap->ClearRequests();
                        L().Info("owner_uid=%u cleared queue", e.uid);
                    } else {
                        ap->SetNotice("denied", "只有房主可以清空队列");
                        L().Info("owner command denied: uid=%u owner=%u \"%s\"", e.uid,
                                 ap->owner_uid, t.c_str());
                    }
                    return;
                }

                // 删歌 / 删除歌曲 / 删除点歌 / 删歌曲（长前缀优先）
                static const char* kDel[] = {"删除歌曲", "删除点歌", "删歌曲", "删歌"};
                for (const char* d : kDel) {
                    size_t dn = strlen(d);
                    if (t.compare(0, dn, d) != 0) continue;
                    if (ap->owner_uid == 0 || e.uid != ap->owner_uid) {
                        ap->SetNotice("denied", "只有房主可以删歌");
                        L().Info("owner command denied: uid=%u owner=%u \"%s\"", e.uid,
                                 ap->owner_uid, t.c_str());
                        return;
                    }
                    ap->OwnerRemoveRequest(trim(t.substr(dn)));
                    return;
                }

                // 随机点歌：随机点歌 [条件] / 点歌 随机 [条件]
                static const char* kRandPrefix[] = {"随机点歌", "!随机点歌", "/随机点歌",
                                                    "！随机点歌"};
                for (const char* p : kRandPrefix) {
                    size_t plen = strlen(p);
                    if (t.compare(0, plen, p) != 0) continue;
                    ap->AddRandomRequest(e.user, e.uid, trim(t.substr(plen)));
                    return;
                }

                static const char* kPrefix[] = {"点歌", "!点歌", "/点歌", "！点歌"};
                for (const char* p : kPrefix) {
                    size_t plen = strlen(p);
                    if (t.compare(0, plen, p) != 0) continue;
                    std::string q = trim(t.substr(plen));
                    if (q.compare(0, 6, "随机") == 0) {
                        ap->AddRandomRequest(e.user, e.uid, trim(q.substr(6)));
                    } else if (!q.empty()) {
                        ap->AddSongRequest(e.user, e.uid, q);
                    }
                    break;
                }
            };
            L().Info("danmaku: room %u protover=%d connecting", rid, bc.protover);
            // 弹幕连接与点歌板解耦：只由 [danmaku] enabled 控制
            bili::Start(bc, dmCb);
        }
        if (reqOn && !dmuOn)
            L().Info("songrequest: enabled but danmaku disabled, no requests will arrive");
    }

    // —— 主采样循环 ——
    uint32_t lastResolve = 0;
    uint32_t lastSongScan = 0;
    uint32_t lastDiffScan = 0;
    uint32_t diffScanUntil = 0;
    uint32_t lastFlBase = 0;
    uint32_t lastFlSeq = 0;
    uint32_t lastSelectCheck = 0;
    bool prevInGame = false;
    uint32_t prevMusicId = 0;
    Snapshot local;
    while (!g_stop.load()) {
        if (!demo) {
            if (app.request_enabled) {
                uint32_t now = now_ms();
                if (now - lastSelectCheck >= 1000) {
                    lastSelectCheck = now;
                    app.ExpireSelection();
                }
            }
            // 字符串签名扫描较耗时（~1秒/次）。只要四个判定字段还没全部解析成功，
            // 就每 2 秒重扫一次；全部解析后缓存地址（同一会话内地址稳定）。
            if (!profile.JudgesOk()) {
                uint32_t now = now_ms();
                if (now - lastResolve >= 2000) {
                    lastResolve = now;
                    profile.Resolve(scanner);
                    app.snap.profile_ok = profile.AnyOk();
                }
            }
            // 曲目结构（换歌后地址会变）：每 10 秒重扫一次
            {
                uint32_t now = now_ms();
                if (now - lastSongScan >= 10000) {
                    lastSongScan = now;
                    profile.RescanSong();
                }
            }
            // FAST/LATE：hook 内部自动维护主对象，离开对局时清零
            if (hook::FastLateInstalled()) {
                if (prevInGame && !local.in_game) hook::ResetFastLate();
            }

            // 当前难度：只在每局开头扫几遍（约 6 秒窗口），之后本局不再扫描
            if (wantDiffScan) {                uint32_t now = now_ms();
                if (local.in_game && !prevInGame) {
                    diffScanUntil = now + 6000;
                    lastDiffScan = 0;
                }
                if (diffScanUntil && now < diffScanUntil && (now - lastDiffScan >= 1500)) {
                    lastDiffScan = now;
                    profile.RefreshDifficulty();
                } else if (diffScanUntil && now >= diffScanUntil) {
                    diffScanUntil = 0;   // 窗口结束，本局不再扫
                }
            }
            local = Snapshot{};
            profile.Sample(local);
            local.profile_name = profileName;
            local.plugin_version = app.version;
            local.db_size = (uint32_t)app.db.size();
            local.demo = false;
            // 进入游玩：标记对应点歌为「正在游玩」；回到选歌：移除该点歌
            if (app.request_enabled) {
                if (local.in_game) {
                    if (!prevInGame ||
                        (local.music_id != 0 && local.music_id != prevMusicId)) {
                        prevMusicId = local.music_id;
                        app.OnSongStarted(local.music_id);
                    }
                } else {
                    if (prevInGame) app.OnSongEnded();
                    prevMusicId = 0;
                }
            }
            if (!profile.AnyOk()) {
                local.hint = "尚未定位到判定数据（内存签名扫描失败）。"
                             "请确认 profile 配置与游戏版本匹配，详见 README 校准章节。";
            } else if (!dbHint.empty()) {
                local.hint = dbHint;
            }
            {
                std::lock_guard<std::mutex> g(app.mtx);
                app.snap = local;
            }
            prevInGame = local.in_game;
        }
        Sleep((DWORD)pollMs);
    }

    g_http.Stop();
    if (demoThread.joinable()) demoThread.join();
    L().Info("worker thread exit");
    if (g_workerDone) SetEvent(g_workerDone);
}

static bool EnsureStarted() {
    if (g_started.exchange(true)) return true;
    g_stop = false;
    if (!g_workerDone) g_workerDone = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_worker = std::thread(&WorkerThread);
    return true;
}

} // namespace csk

// —— ASI 导出 ——
extern "C" __declspec(dllexport) void InitializeASI() {
    csk::EnsureStarted();
}

extern "C" __declspec(dllexport) const char* GetCSKVersion() {
    return "chunithm-streamkit 1.0.6";
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        csk::g_module = h;
        std::set_terminate(&csk::TerminateHandler);
        DisableThreadLibraryCalls(h);
        csk::BootTrace("dllmain attach");
        // 极早安装分数 hook：此时游戏主逻辑尚未运行，从根本上避开
        // 「改写指令时游戏线程正好执行到该地址」的竞态闪退。
        csk::hook::Install();
        csk::BootTrace("early score hook done");
        csk::EnsureStarted();
        csk::BootTrace("dllmain attach done");
    } else if (reason == DLL_PROCESS_DETACH) {
        // 优雅停机：停服务器、发停止信号、等待工作线程收尾。
        // （不 join std::thread —— 事件等待避免加载器锁场景下的隐患。）
        csk::g_stop = true;
        csk::g_http.Stop();
        if (csk::g_workerDone) WaitForSingleObject(csk::g_workerDone, 3000);
    }
    return TRUE;
}
