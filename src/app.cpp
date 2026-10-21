// chunithm-streamkit — app.cpp
#include "app.h"
#include "util.h"
#include "hook.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <random>
#include <algorithm>

namespace csk {

static void AppendGameJson(std::string& s, const Snapshot& snap) {
    char buf[64];
    s += "\"in_game\":" + std::string(snap.in_game ? "true" : "false") + ",";
    s += "\"status\":\"" + json_escape(snap.status) + "\",";
    s += "\"music_id\":" + std::to_string(snap.music_id) + ",";
    s += "\"difficulty\":" + std::to_string(snap.difficulty) + ",";
    s += "\"score\":" + std::to_string(snap.score) + ",";
    s += "\"combo\":" + std::to_string(snap.combo) + ",";
    s += "\"max_combo\":" + std::to_string(snap.max_combo) + ",";
    s += "\"note_total\":" + std::to_string(snap.note_total) + ",";
    s += "\"justice_crit\":" + std::to_string(snap.jc) + ",";
    s += "\"justice\":" + std::to_string(snap.ju) + ",";
    s += "\"attack\":" + std::to_string(snap.at) + ",";
    s += "\"miss\":" + std::to_string(snap.mi) + ",";
    s += "\"fast\":" + std::to_string(snap.fast) + ",";
    s += "\"late\":" + std::to_string(snap.late) + ",";
    s += "\"fast_jc\":" + std::to_string(snap.fast_jc) + ",";
    s += "\"late_jc\":" + std::to_string(snap.late_jc) + ",";
    s += "\"fast_attack\":" + std::to_string(snap.fast_at) + ",";
    s += "\"late_attack\":" + std::to_string(snap.late_at);
}

std::string App::BuildEventJson(const JudgeEvent& ev) {
    char buf[64];
    std::string s = "{\"type\":\"event\",\"seq\":" + std::to_string(ev.seq);
    s += ",\"t\":" + std::to_string(ev.t_ms);
    s += ",\"judge\":\"" + std::string(JudgeName(ev.type)) + "\"";
    s += ",\"lane\":" + std::to_string(ev.lane);
    s += ",\"delta_ms\":" + std::to_string(ev.delta_ms);
    s += "}";
    (void)buf;
    return s;
}

// ---- 点歌队列 ----

// 把 “master” / “[MASTER]” / “大师” 这类尾部标记解析成难度下标
static int DifficultyFromToken(const std::string& raw) {
    std::string t;
    for (unsigned char c : raw) {
        if (c == ' ' || c == '\t' || c == '[' || c == ']' || c == '(' || c == ')' ||
            c == '-' || c == '_' || c == '/' || c == '\\' || c == '"' || c == '\'')
            continue;
        if (c < 0x80) t += (char)tolower(c);
        else t += (char)c;
    }
    if (t == "basic" || t == "bsc" || t == "基础" || t == "绿") return 0;
    if (t == "advanced" || t == "adv" || t == "高级" || t == "黄") return 1;
    if (t == "expert" || t == "exp" || t == "专家" || t == "红") return 2;
    if (t == "master" || t == "mst" || t == "大师" || t == "紫") return 3;
    if (t == "ultima" || t == "ult" || t == "究极" || t == "黑") return 4;
    if (t == "we" || t == "worldsend" || t == "世界末日") return 5;
    return -1;
}

// 成功时从 query 中去掉难度标记，返回 0..5；未指定返回 -1
static int TakeDifficulty(std::string& query) {
    std::string q = trim(query);
    if (q.empty()) return -1;

    if (q.back() == ']' || q.back() == ')') {
        size_t b = q.find_last_of(q.back() == ']' ? "[" : "(");
        if (b != std::string::npos && b + 1 < q.size() - 1) {
            int d = DifficultyFromToken(q.substr(b + 1, q.size() - b - 2));
            if (d >= 0) {
                std::string rest = trim(q.substr(0, b));
                if (!rest.empty()) {
                    query = rest;
                    return d;
                }
            }
        }
    }

    size_t sp = q.find_last_of(" \t");
    std::string tok = (sp == std::string::npos) ? q : q.substr(sp + 1);
    int d = DifficultyFromToken(tok);
    if (d >= 0) {
        std::string rest = (sp == std::string::npos) ? std::string() : trim(q.substr(0, sp));
        if (!rest.empty()) {
            query = rest;
            return d;
        }
    }
    return -1;
}

// 前置难度：'紫' 'larva' / '紫larva' / '[MASTER] larva'
static int TakeLeadingDifficulty(std::string& query) {
    std::string q = trim(query);
    if (q.empty()) return -1;

    // [MASTER] / (MASTER) 开头
    if (q[0] == '[' || q[0] == '(') {
        char close = (q[0] == '[') ? ']' : ')';
        size_t e = q.find(close);
        if (e != std::string::npos) {
            int d = DifficultyFromToken(q.substr(1, e - 1));
            std::string rest = trim(q.substr(e + 1));
            if (d >= 0 && !rest.empty()) {
                query = rest;
                return d;
            }
        }
    }

    // 空白分隔的第一个 token（含 '紫' / "MASTER" 这类带引号的写法）
    size_t sp = q.find_first_of(" \t");
    if (sp != std::string::npos) {
        int d = DifficultyFromToken(q.substr(0, sp));
        std::string rest = trim(q.substr(sp + 1));
        if (d >= 0 && !rest.empty()) {
            query = rest;
            return d;
        }
    }

    // 紧挨着写：中文单字颜色 + 曲名（中文/英文/数字都允许），
    // 若整段本身是歌名，会在 AddSongRequest 里优先按整段匹配。
    static const char* kColors[] = {"绿", "黄", "红", "紫", "黑"};
    for (int i = 0; i < 5; ++i) {
        size_t n = strlen(kColors[i]);
        if (q.compare(0, n, kColors[i]) != 0 || q.size() <= n) continue;
        std::string rest = trim(q.substr(n));
        if (!rest.empty()) {
            query = rest;
            return i;
        }
    }
    return -1;
}

static const char* kDiffNames[6] = {"BASIC", "ADVANCED", "EXPERT",
                                    "MASTER", "ULTIMA", "WORLD'S END"};

static bool HasDifficulty(const SongInfo& si, int d) {
    if (d < 0 || d > 5) return false;
    if (d == 5) return si.diff_const[5] > 0 || !si.we_tag.empty();
    return si.diff_const[d] > 0;
}

void App::ConfigureRequests(bool enabled, int maxQueue) {
    std::lock_guard<std::mutex> g(mtx);
    request_enabled = enabled;
    request_max = maxQueue < 1 ? 1 : (maxQueue > 30 ? 30 : maxQueue);
}

void App::SetRequestsEnabled(bool enabled) {
    std::lock_guard<std::mutex> g(mtx);
    request_enabled = enabled;
    if (!enabled) {
        CancelPendingLocked();
    }
    SetNoticeLocked(enabled ? "enabled" : "disabled",
                    enabled ? "点歌已开启" : "点歌已关闭");
    L().Info("songrequest %s at runtime", enabled ? "enabled" : "disabled");
}

void App::SetQueuePath(const std::string& path) { request_path = path; }

static std::string TsvSafe(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    }
    return out;
}

void App::SaveQueueLocked() {
    if (request_path.empty() || !request_enabled) return;
    std::string tmp = request_path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fputs("# csk-songrequest-v1\n", f);
    for (const auto& r : requests) {
        fprintf(f, "%llu\t%u\t%u\t%s\t%s\t%s\t%s\t%d\t%u\n",
                (unsigned long long)r.id, r.song_id, r.uid, TsvSafe(r.title).c_str(),
                TsvSafe(r.artist).c_str(), TsvSafe(r.user).c_str(),
                TsvSafe(r.query).c_str(), r.difficulty, r.t_ms);
    }
    fclose(f);
    MoveFileExA(tmp.c_str(), request_path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void App::LoadQueue() {
    std::lock_guard<std::mutex> g(mtx);
    if (!request_enabled || request_path.empty()) return;
    FILE* f = fopen(request_path.c_str(), "rb");
    if (!f) return;
    requests.clear();
    request_seq = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        auto cols = split(s, '\t');
        if (cols.size() < 9) continue;
        SongRequest r;
        r.id = parse_u64(cols[0]);
        r.song_id = (uint32_t)parse_u64(cols[1]);
        r.uid = (unsigned)parse_u64(cols[2]);
        r.title = cols[3];
        r.artist = cols[4];
        r.user = cols[5];
        r.query = cols[6];
        r.difficulty = parse_i32(cols[7], -1);
        if (r.difficulty < -1 || r.difficulty > 5) r.difficulty = -1;
        r.t_ms = (uint32_t)parse_u64(cols[8]);
        r.playing = false;   // 启动时不可能在游玩，重置高亮
        if (r.id > request_seq) request_seq = r.id;
        requests.push_back(std::move(r));
        if ((int)requests.size() >= request_max) break;
    }
    fclose(f);
    L().Info("songrequest: loaded %zu request(s) from %s", requests.size(),
             request_path.c_str());
}

std::string App::QueueCandidateLocked(const RequestCandidate& c, const std::string& user,
                                      unsigned uid, const std::string& query, int diff,
                                      uint32_t t) {
    for (const auto& r : requests) {
        if (r.song_id == c.song_id && r.difficulty == diff) {
            SetNoticeLocked("duplicate", "已在队列：" + c.title);
            request_last.title = c.title;
            L().Info("request <%s> \"%s\" -> duplicate [%u] %s", user.c_str(), query.c_str(),
                     c.song_id, c.title.c_str());
            return "duplicate";
        }
    }
    if ((int)requests.size() >= request_max) {
        SetNoticeLocked("full", "队列已满：" + c.title);
        request_last.title = c.title;
        L().Info("request <%s> \"%s\" -> queue full (%d)", user.c_str(), query.c_str(),
                 request_max);
        return "full";
    }

    SongRequest r;
    r.id = ++request_seq;
    r.song_id = c.song_id;
    r.uid = uid;
    r.title = c.title;
    r.artist = c.artist;
    r.user = user;
    r.query = query;
    r.difficulty = diff;
    r.t_ms = t;
    requests.push_back(r);
    SaveQueueLocked();

    SetNoticeLocked("added", "已加入：" + c.title);
    request_last.title = c.title;
    std::string diffTag = diff >= 0 ? (" [" + std::string(kDiffNames[diff]) + "]") : "";
    L().Info("request #%llu <%s> \"%s\"%s -> [%u] %s", (unsigned long long)r.id, user.c_str(),
             query.c_str(), diffTag.c_str(), c.song_id, c.title.c_str());
    return "added";
}

std::string App::AddSongRequest(const std::string& user, unsigned uid,
                                const std::string& query) {
    std::lock_guard<std::mutex> g(mtx);
    std::string q = trim(query);
    std::string rawQuery = q;
    int diff = TakeDifficulty(q);
    bool leadingDiff = false;
    if (diff < 0) {
        diff = TakeLeadingDifficulty(q);
        leadingDiff = (diff >= 0);
    }
    request_last = RequestNotice{};
    request_last.user = user;
    request_last.query = q;
    request_last.t_ms = now_ms();

    if (!request_enabled) {
        request_last.status = "disabled";
        request_last.text = "点歌功能未开启";
        return "disabled";
    }
    if (pending.active) {
        request_last.status = "busy";
        request_last.text = "有人正在选歌，请稍后再点";
        return "busy";
    }
    if (q.empty()) {
        request_last.status = "not_found";
        request_last.text = "请输入曲名";
        return "not_found";
    }

    // 前置难度拆分后，如果整段本身就是一个精确曲名/别名，优先按整段匹配
    // 例：紫苑 → 歌名；紫创造 → MASTER + 创造
    if (leadingDiff && q != rawQuery) {
        auto whole = db.FindCandidates(rawQuery, 8);
        if (!whole.empty() && whole[0].score >= 950) {
            q = rawQuery;
            diff = -1;
        }
    }
    request_last.query = q;

    auto matches = db.FindCandidates(q, 8);
    // 难度词可能是标题的一部分（例如曲名本身以「紫」开头且后面是 ASCII）：
    // 去掉难度后找不到歌时，回退为「整段当曲名、不指定难度」
    if (matches.empty() && diff >= 0 && q != rawQuery) {
        auto alt = db.FindCandidates(rawQuery, 8);
        if (!alt.empty()) {
            matches.swap(alt);
            q = rawQuery;
            diff = -1;
        }
    }
    if (diff >= 0) {
        std::vector<SongDb::SongMatch> filtered;
        for (const auto& m : matches) {
            if (HasDifficulty(*m.song, diff)) filtered.push_back(m);
        }
        if (filtered.empty()) {
            if (!matches.empty()) {
                request_last.status = "diff_missing";
                request_last.text = "该曲没有 " + std::string(kDiffNames[diff]) + " 难度";
                request_last.title = matches[0].song->title;
                L().Info("request <%s> \"%s\" -> no %s difficulty", user.c_str(), q.c_str(),
                         kDiffNames[diff]);
                return "diff_missing";
            }
            request_last.status = "not_found";
            request_last.text = "未找到：" + q;
            return "not_found";
        }
        matches.swap(filtered);
    }
    if (matches.empty()) {
        request_last.status = "not_found";
        request_last.text = "未找到：" + q;
        L().Info("request <%s> \"%s\" -> not found", user.c_str(), q.c_str());
        return "not_found";
    }
    if ((int)requests.size() >= request_max) {
        request_last.status = "full";
        request_last.title = matches[0].song->title;
        request_last.text = "队列已满：" + matches[0].song->title;
        return "full";
    }

    auto makeCandidate = [](const SongInfo& si) {
        RequestCandidate c;
        c.song_id = si.id;
        c.title = si.title;
        c.artist = si.artist;
        c.consts = si.diff_const;
        c.we_tag = si.we_tag;
        return c;
    };

    // 多个分数接近的结果：进入选择状态，由点歌人回复数字
    bool ambiguous = matches.size() >= 2 &&
                     (matches[1].score >= matches[0].score - 200 ||
                      (matches[0].score < 1000 && matches[1].score >= 380));
    if (!ambiguous) return QueueCandidateLocked(makeCandidate(*matches[0].song), user, uid, q,
                                                diff, request_last.t_ms);

    pending.active = true;
    pending.uid = uid;
    pending.user = user;
    pending.query = q;
    pending.difficulty = diff;
    pending.t_ms = now_ms();
    pending.candidates.clear();
    for (size_t i = 0; i < matches.size() && i < 4; ++i)
        pending.candidates.push_back(makeCandidate(*matches[i].song));

    request_last.status = "select";
    request_last.text = "找到 " + std::to_string(pending.candidates.size()) +
                        " 个结果，请回复 1-" + std::to_string(pending.candidates.size()) +
                        " 选择";
    request_last.title = q;
    L().Info("request <%s> \"%s\" -> %zu candidates, waiting for choice", user.c_str(),
             q.c_str(), pending.candidates.size());
    return "select";
}

static std::string UpperAscii(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

static bool ParseDoubleAll(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = strtod(s.c_str(), &end);
    return end != s.c_str() && end && *end == '\0';
}

// 14 / 14.5 / 14+ / 13-15 / >=14
static bool ParseLevelToken(const std::string& tok, double& lo, double& hi) {
    std::string t = trim(tok);
    if (t.empty()) return false;
    if (t.compare(0, 2, ">=") == 0) {
        double v = 0;
        if (!ParseDoubleAll(trim(t.substr(2)), v)) return false;
        lo = v;
        hi = 99;
        return true;
    }
    if (t[0] == '>') {
        double v = 0;
        if (!ParseDoubleAll(trim(t.substr(1)), v)) return false;
        lo = v + 0.001;
        hi = 99;
        return true;
    }
    if (t.compare(0, 2, "<=") == 0) {
        double v = 0;
        if (!ParseDoubleAll(trim(t.substr(2)), v)) return false;
        lo = 0;
        hi = v;
        return true;
    }
    size_t dash = t.find('-');
    if (dash != std::string::npos) {
        double a = 0, b = 0;
        if (!ParseDoubleAll(trim(t.substr(0, dash)), a)) return false;
        if (!ParseDoubleAll(trim(t.substr(dash + 1)), b)) return false;
        lo = a;
        hi = b;
        return true;
    }
    if (t.back() == '+') {
        double v = 0;
        if (!ParseDoubleAll(trim(t.substr(0, t.size() - 1)), v)) return false;
        lo = v + 0.5;
        hi = v + 1.0;
        return true;
    }
    double v = 0;
    if (!ParseDoubleAll(t, v)) return false;
    lo = v;
    hi = (std::floor(v) == v) ? v + 1.0 : v + 0.001;
    return true;
}

static bool ContainsNoCase(const std::string& hay, const std::string& needle) {
    return UpperAscii(hay).find(UpperAscii(needle)) != std::string::npos;
}

static std::string VersionKey(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == ' ' || c == '-' || c == '_' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out += c;
    }
    return out;
}

static bool VersionEquals(const SongInfo& si, const std::string& tok) {
    if (si.version.empty()) return false;
    return VersionKey(si.version) == VersionKey(tok);
}

std::string App::AddRandomRequest(const std::string& user, unsigned uid,
                                  const std::string& args) {
    std::lock_guard<std::mutex> g(mtx);
    std::string cond = trim(args);
    if (cond.compare(0, 6, "随机") == 0) cond = trim(cond.substr(6));

    request_last = RequestNotice{};
    request_last.user = user;
    request_last.query = cond.empty() ? "随机" : cond;
    request_last.t_ms = now_ms();

    if (!request_enabled) {
        request_last.status = "disabled";
        request_last.text = "点歌功能未开启";
        return "disabled";
    }
    if (pending.active) {
        request_last.status = "busy";
        request_last.text = "有人正在选歌，请稍后再点";
        return "busy";
    }
    if ((int)requests.size() >= request_max) {
        request_last.status = "full";
        request_last.text = "队列已满";
        return "full";
    }

    struct Filter {
        int diff = -1;
        double lvMin = -1, lvMax = -1;
        std::string genre;
        std::vector<std::string> versions;   // 可多个：MATE VERSE = 两个版本
    } f;

    // 整段扫描版本名（忽略空格/连字符）：XVERSE = X-VERSE，XVERSEX = X-VERSE-X
    std::string scan = VersionKey(cond);
    static const char* kVersionNames[] = {
        "PARADISE LOST", "LUMINOUS PLUS", "AMAZON PLUS", "CRYSTAL PLUS", "STAR PLUS",
        "SUN PLUS",      "NEW PLUS",      "AIR PLUS",    "X-VERSE-X",    "X-VERSE",
        "MATE",          "CHUNITHM",      "AIR",         "STAR",         "AMAZON",
        "CRYSTAL",       "PARADISE",      "NEW",         "SUN",          "LUMINOUS",
        "VERSE",         "PLUS",
    };
    for (const char* v : kVersionNames) {
        std::string key = VersionKey(v);
        size_t pos;
        while ((pos = scan.find(key)) != std::string::npos) {
            if (std::find(f.versions.begin(), f.versions.end(), v) == f.versions.end())
                f.versions.push_back(v);
            scan.erase(pos, key.size());
        }
    }
    // 一次只认一个版本；多写时取第一个（不把 MATE VERSE 当成组合规则）
    if (f.versions.size() > 1) f.versions.resize(1);
    auto isVersionWord = [](const std::string& u) {
        static const char* kWords[] = {
            "PARADISE", "LOST",   "LUMINOUS", "AMAZON", "CRYSTAL", "STAR",
            "SUN",      "NEW",    "AIR",      "X",      "X-VERSE", "X-VERSE-X",
            "MATE",     "VERSE",  "PLUS",     "CHUNITHM",
        };
        std::string key = VersionKey(u);
        for (const char* w : kWords)
            if (key == VersionKey(w)) return true;
        return false;
    };

    auto toks = split(cond, ' ');
    for (const auto& raw : toks) {
        std::string tok = trim(raw);
        if (tok.empty() || tok == "随机") continue;
        int d = DifficultyFromToken(tok);
        if (d >= 0) {
            f.diff = d;
            continue;
        }
        double lo = 0, hi = 0;
        if (ParseLevelToken(tok, lo, hi)) {
            f.lvMin = lo;
            f.lvMax = hi;
            continue;
        }
        std::string up = UpperAscii(tok);
        if (isVersionWord(up)) continue;
        f.genre = tok;
    }

    auto songs = db.SnapshotSongs();
    std::vector<const SongInfo*> pool;
    std::vector<int> poolDiff;
    for (const auto& si : songs) {
        bool queued = false;
        for (const auto& r : requests) {
            if (r.song_id == si.id) {
                queued = true;
                break;
            }
        }
        if (queued) continue;
        int chosen = -1;
        if (f.diff >= 0) {
            if (!HasDifficulty(si, f.diff)) continue;
            if (f.lvMin >= 0) {
                double c = si.diff_const[f.diff];
                if (!(c > 0 && c >= f.lvMin && c <= f.lvMax)) continue;
            }
            chosen = f.diff;
        } else if (f.lvMin >= 0) {
            for (int i = 0; i < 5; ++i) {
                double c = si.diff_const[i];
                if (c > 0 && c >= f.lvMin && c <= f.lvMax) {
                    chosen = i;
                    break;
                }
            }
            if (chosen < 0) continue;
        }
        if (!f.genre.empty() && !ContainsNoCase(si.genre, f.genre)) continue;
        if (!f.versions.empty()) {
            bool ok = false;
            for (const auto& v : f.versions) {
                if (VersionEquals(si, v)) {
                    ok = true;
                    break;
                }
            }
            if (!ok) continue;
        }
        pool.push_back(&si);
        poolDiff.push_back(chosen);
    }

    if (pool.empty()) {
        request_last.status = "not_found";
        request_last.text = "没有符合条件的歌";
        L().Info("random request <%s> \"%s\" -> no match", user.c_str(), cond.c_str());
        return "not_found";
    }

    std::mt19937 rng((uint32_t)std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, pool.size() - 1);
    size_t idx = dist(rng);
    const SongInfo& si = *pool[idx];
    RequestCandidate c;
    c.song_id = si.id;
    c.title = si.title;
    c.artist = si.artist;
    c.consts = si.diff_const;
    c.we_tag = si.we_tag;
    L().Info("random request <%s> \"%s\" -> [%u] %s", user.c_str(), cond.c_str(), si.id,
             si.title.c_str());
    return QueueCandidateLocked(c, user, uid, cond.empty() ? "随机" : cond, poolDiff[idx],
                                request_last.t_ms);
}

bool App::HandleSelection(unsigned uid, int number) {    std::lock_guard<std::mutex> g(mtx);
    if (!pending.active || pending.uid != uid) return false;
    if (number < 1 || number > (int)pending.candidates.size()) {
        SetNoticeLocked("select", "请输入 1-" + std::to_string(pending.candidates.size()));
        return true;
    }
    RequestCandidate c = pending.candidates[number - 1];
    std::string user = pending.user, q = pending.query;
    int diff = pending.difficulty;
    uint32_t t = pending.t_ms;
    pending = PendingSelection{};
    QueueCandidateLocked(c, user, uid, q, diff, t);
    return true;
}

void App::ExpireSelection() {
    std::lock_guard<std::mutex> g(mtx);
    if (!pending.active) return;
    if (now_ms() - pending.t_ms < 60000) return;
    pending = PendingSelection{};
    SetNoticeLocked("timeout", "选择超时，请重新点歌");
    L().Info("request selection timed out");
}

void App::SetNoticeLocked(const std::string& status, const std::string& text) {
    request_last = RequestNotice{};
    request_last.status = status;
    request_last.text = text;
    request_last.t_ms = now_ms();
}

void App::CancelPendingLocked() { pending = PendingSelection{}; }

int App::CancelRequestsByUser(unsigned uid, const std::string& user) {
    std::lock_guard<std::mutex> g(mtx);
    int removed = 0;
    std::string lastTitle;
    bool canceledPending =
        pending.active && ((uid != 0 && pending.uid == uid) || (uid == 0 && pending.user == user));
    if (canceledPending) pending = PendingSelection{};
    for (auto it = requests.begin(); it != requests.end();) {
        bool mine = (uid != 0) ? (it->uid == uid) : (it->user == user);
        if (mine) {
            lastTitle = it->title;
            it = requests.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) SaveQueueLocked();
    request_last = RequestNotice{};
    request_last.user = user;
    request_last.t_ms = now_ms();
    if (removed == 0) {
        if (canceledPending) {
            request_last.status = "cancelled";
            request_last.text = "已取消你的点歌选择";
        } else {
            request_last.status = "no_request";
            request_last.text = "没有找到你的点歌";
        }
    } else {
        request_last.status = "cancelled";
        request_last.title = lastTitle;
        request_last.text = removed == 1 ? ("已取消：" + lastTitle)
                                         : ("已取消 " + std::to_string(removed) + " 首点歌");
        L().Info("cancel <%s uid=%u> removed %d request(s)", user.c_str(), uid, removed);
    }
    return removed;
}

std::string App::OwnerRemoveRequest(const std::string& arg) {
    std::lock_guard<std::mutex> g(mtx);
    std::string a = trim(arg);
    request_last = RequestNotice{};
    request_last.t_ms = now_ms();
    request_last.query = a;

    if (a.empty()) {
        request_last.status = "usage";
        request_last.text = "用法：删歌 <序号或曲名>";
        return "usage";
    }

    auto report = [&](const char* status, const std::string& text,
                      const std::string& title) {
        request_last.status = status;
        request_last.text = text;
        request_last.title = title;
        L().Info("owner remove: %s", text.c_str());
        return std::string(status);
    };

    bool digits = true;
    for (char c : a) {
        if (c < '0' || c > '9') { digits = false; break; }
    }
    if (digits) {
        int idx = atoi(a.c_str());
        if (idx >= 1 && idx <= (int)requests.size()) {
            std::string title = requests[idx - 1].title;
            requests.erase(requests.begin() + (idx - 1));
            SaveQueueLocked();
            return report("removed", "已删除：" + title, title);
        }
        return report("not_found", "序号超出范围：" + a, "");
    }

    const SongInfo* si = db.FindBest(a);
    for (auto it = requests.begin(); it != requests.end(); ++it) {
        if ((si && it->song_id == si->id) || it->title.find(a) != std::string::npos) {
            std::string title = it->title;
            requests.erase(it);
            SaveQueueLocked();
            return report("removed", "已删除：" + title, title);
        }
    }
    return report("not_found", "队列里没有：" + a, "");
}

bool App::RemoveRequest(uint64_t id) {
    std::lock_guard<std::mutex> g(mtx);
    for (auto it = requests.begin(); it != requests.end(); ++it) {
        if (it->id == id) {
            requests.erase(it);
            SaveQueueLocked();
            return true;
        }
    }
    return false;
}

bool App::MoveRequest(uint64_t id, int toIndex) {
    std::lock_guard<std::mutex> g(mtx);
    size_t from = requests.size();
    for (size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].id == id) {
            from = i;
            break;
        }
    }
    if (from == requests.size()) return false;
    int to = toIndex;
    if (to < 0) to = 0;
    if (to >= (int)requests.size()) to = (int)requests.size() - 1;
    if ((int)from == to) return true;
    SongRequest r = requests[from];
    requests.erase(requests.begin() + (std::ptrdiff_t)from);
    requests.insert(requests.begin() + to, r);
    SaveQueueLocked();
    return true;
}

bool App::MovePendingCandidate(int from, int to) {
    std::lock_guard<std::mutex> g(mtx);
    if (!pending.active) return false;
    int n = (int)pending.candidates.size();
    if (from < 0 || from >= n || to < 0 || to >= n) return false;
    if (from == to) return true;
    RequestCandidate c = pending.candidates[from];
    pending.candidates.erase(pending.candidates.begin() + from);
    pending.candidates.insert(pending.candidates.begin() + to, c);
    return true;
}

bool App::RemovePendingCandidate(int index) {
    std::lock_guard<std::mutex> g(mtx);
    if (!pending.active) return false;
    int n = (int)pending.candidates.size();
    if (index < 0 || index >= n) return false;
    pending.candidates.erase(pending.candidates.begin() + index);
    if (pending.candidates.empty()) {
        pending = PendingSelection{};
        SetNoticeLocked("timeout", "候选已清空，请重新点歌");
    }
    return true;
}

bool App::ChoosePendingCandidate(int index) {
    std::lock_guard<std::mutex> g(mtx);
    if (!pending.active) return false;
    int n = (int)pending.candidates.size();
    if (index < 0 || index >= n) return false;
    RequestCandidate c = pending.candidates[index];
    std::string user = pending.user, q = pending.query;
    unsigned uid = pending.uid;
    int diff = pending.difficulty;
    uint32_t t = pending.t_ms;
    pending = PendingSelection{};
    QueueCandidateLocked(c, user, uid, q, diff, t);
    return true;
}

void App::ClearRequests() {
    std::lock_guard<std::mutex> g(mtx);
    requests.clear();
    CancelPendingLocked();
    SaveQueueLocked();
    request_last = RequestNotice{};
    request_last.status = "cleared";
    request_last.text = "队列已清空";
    request_last.t_ms = now_ms();
}

void App::SetNotice(const std::string& status, const std::string& text) {
    std::lock_guard<std::mutex> g(mtx);
    SetNoticeLocked(status, text);
}

void App::OnSongStarted(uint32_t musicId) {
    std::lock_guard<std::mutex> g(mtx);
    for (auto& r : requests) r.playing = false;
    if (musicId == 0) return;

    size_t idx = requests.size();
    for (size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].song_id == musicId) {
            idx = i;
            break;
        }
    }
    if (idx == requests.size()) return;

    SongRequest req = requests[idx];
    requests.erase(requests.begin() + (std::ptrdiff_t)idx);
    req.playing = true;
    requests.push_front(req);
    SaveQueueLocked();
    L().Info("request #%llu now playing, moved to front ([%u] %s)",
             (unsigned long long)req.id, musicId, req.title.c_str());
}

void App::OnSongEnded() {
    std::lock_guard<std::mutex> g(mtx);
    bool removed = false;
    for (auto it = requests.begin(); it != requests.end();) {
        if (it->playing) {
            L().Info("request #%llu finished, removed from queue ([%u] %s)",
                     (unsigned long long)it->id, it->song_id, it->title.c_str());
            it = requests.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed) SaveQueueLocked();
}

std::string App::BuildRequestsJsonLocked() {
    std::string s = "{\"enabled\":";
    s += request_enabled ? "true" : "false";
    s += ",\"max\":" + std::to_string(request_max);
    s += ",\"items\":[";
    bool first = true;
    for (const auto& r : requests) {
        if (!first) s += ",";
        first = false;
        s += "{\"id\":" + std::to_string(r.id);
        s += ",\"song_id\":" + std::to_string(r.song_id);
        s += ",\"title\":\"" + json_escape(r.title) + "\"";
        s += ",\"artist\":\"" + json_escape(r.artist) + "\"";
        s += ",\"user\":\"" + json_escape(r.user) + "\"";
        s += ",\"query\":\"" + json_escape(r.query) + "\"";
        s += ",\"difficulty\":" + std::to_string(r.difficulty);
        const SongInfo* si = db.FindById(r.song_id);
        s += ",\"const\":";
        if (si && r.difficulty >= 0 && r.difficulty < 6 &&
            si->diff_const[r.difficulty] > 0) {
            char cb[32];
            snprintf(cb, sizeof(cb), "%.1f", si->diff_const[r.difficulty]);
            s += cb;
        } else {
            s += "null";
        }
        s += ",\"genre\":\"" + json_escape(si ? si->genre : "") + "\"";
        s += ",\"version\":\"" + json_escape(si ? si->version : "") + "\"";
        s += ",\"playing\":" + std::string(r.playing ? "true" : "false");
        s += ",\"t\":" + std::to_string(r.t_ms) + "}";
    }
    s += "]";
    if (!request_last.status.empty()) {
        s += ",\"last\":{\"user\":\"" + json_escape(request_last.user) + "\"";
        s += ",\"query\":\"" + json_escape(request_last.query) + "\"";
        s += ",\"title\":\"" + json_escape(request_last.title) + "\"";
        s += ",\"status\":\"" + json_escape(request_last.status) + "\"";
        s += ",\"msg\":\"" + json_escape(request_last.text) + "\"";
        s += ",\"t\":" + std::to_string(request_last.t_ms) + "}";
    }
    s += "}";
    return s;
}

std::string App::BuildRequestsJson() {
    std::lock_guard<std::mutex> g(mtx);
    return BuildRequestsJsonLocked();
}

std::string App::BuildPendingJsonLocked() {
    std::string s = "{\"active\":";
    if (!pending.active) {
        s += "false}";
        return s;
    }
    s += "true";
    s += ",\"user\":\"" + json_escape(pending.user) + "\"";
    s += ",\"query\":\"" + json_escape(pending.query) + "\"";
    s += ",\"difficulty\":" + std::to_string(pending.difficulty);
    s += ",\"t\":" + std::to_string(pending.t_ms);
    s += ",\"candidates\":[";
    bool first = true;
    for (const auto& c : pending.candidates) {
        if (!first) s += ",";
        first = false;
        s += "{\"song_id\":" + std::to_string(c.song_id);
        s += ",\"title\":\"" + json_escape(c.title) + "\"";
        s += ",\"artist\":\"" + json_escape(c.artist) + "\"";
        s += ",\"consts\":[";
        for (int i = 0; i < 6; ++i) {
            if (i) s += ",";
            if (c.consts[i] > 0) {
                char b[32];
                snprintf(b, sizeof(b), "%.1f", c.consts[i]);
                s += b;
            } else {
                s += "null";
            }
        }
        s += "]";
        const SongInfo* csi = db.FindById(c.song_id);
        s += ",\"genre\":\"" + json_escape(csi ? csi->genre : "") + "\"";
        s += ",\"version\":\"" + json_escape(csi ? csi->version : "") + "\"";
        if (!c.we_tag.empty()) s += ",\"we_tag\":\"" + json_escape(c.we_tag) + "\"";
        s += "}";
    }
    s += "]}";
    return s;
}

void App::AddDanmaku(const std::string& user, const std::string& text) {
    std::lock_guard<std::mutex> g(mtx);
    DanmakuMsg m;
    m.user = user;
    m.text = text;
    m.t_ms = now_ms();
    chat.push_back(std::move(m));
    while (chat.size() > 5) chat.pop_front();
}

std::string App::BuildChatJsonLocked() {
    std::string s = "{\"max\":5,\"items\":[";
    bool first = true;
    for (const auto& m : chat) {
        if (!first) s += ",";
        first = false;
        s += "{\"user\":\"" + json_escape(m.user) + "\"";
        s += ",\"text\":\"" + json_escape(m.text) + "\"";
        s += ",\"t\":" + std::to_string(m.t_ms) + "}";
    }
    s += "]}";
    return s;
}

std::string App::BuildChatJson() {
    std::lock_guard<std::mutex> g(mtx);
    return BuildChatJsonLocked();
}

std::string App::BuildStateJson(bool withFeed) {
    std::lock_guard<std::mutex> g(mtx);
    std::string s = "{\"ok\":true,\"ts\":" + std::to_string(now_ms()) + ",";
    s += "\"demo\":" + std::string(snap.demo ? "true" : "false") + ",";
    s += "\"plugin\":{\"version\":\"" + json_escape(version) + "\",";
    s += "\"profile\":\"" + json_escape(snap.profile_name) + "\",";
    s += "\"signatures\":" + std::string(snap.profile_ok ? "true" : "false") + ",";
    s += "\"db_size\":" + std::to_string(snap.db_size) + ",";
    s += "\"hint\":\"" + json_escape(snap.hint) + "\"},";

    s += "\"song\":{";
    s += "\"obj\":" + std::to_string((unsigned)hook::SongPtr()) + ",";
    // music_id 0 表示「未选曲」（主菜单/加载中），不能当作数据库里的 0 号曲目
    const SongInfo* si = (snap.music_id != 0) ? db.Lookup(snap.music_id) : nullptr;
    if (!si && snap.demo) {
        auto it = demoSongs.find(snap.music_id);
        if (it != demoSongs.end()) si = &it->second;
    }
    if (si && si->known) {
        s += "\"known\":true,";
        s += "\"title\":\"" + json_escape(si->title) + "\",";
        s += "\"artist\":\"" + json_escape(si->artist) + "\",";
        s += "\"bpm\":" + (si->bpm > 0 ? std::to_string(si->bpm) : "null") + ",";
        int d = snap.difficulty;
        std::string dname = (d >= 0 && d <= 5) ? si->diff_names[d] : "UNKNOWN";
        double dc = (d >= 0 && d <= 5) ? si->diff_const[d] : 0.0;
        s += "\"difficulty_name\":\"" + json_escape(dname) + "\",";
        s += "\"const\":";
        if (dc > 0) {
            char cb[32];
            snprintf(cb, sizeof(cb), "%.1f", dc);
            s += cb;
        } else s += "null";
        // 全部难度定数（选歌界面用来排列 6 个小分栏）
        s += ",\"consts\":[";
        for (int i = 0; i < 6; ++i) {
            if (i) s += ",";
            if (si->diff_const[i] > 0) {
                char cb[32];
                snprintf(cb, sizeof(cb), "%.1f", si->diff_const[i]);
                s += cb;
            } else s += "null";
        }
        s += "]";
        if (!si->we_tag.empty()) {
            s += ",\"we_tag\":\"" + json_escape(si->we_tag) + "\"";
        }
    } else {
        s += "\"known\":false,\"title\":null,\"artist\":null,\"bpm\":null,"
             "\"difficulty_name\":null,\"const\":null";
    }
    s += "},";

    s += "\"game\":{";
    AppendGameJson(s, snap);
    s += "},";

    s += "\"request\":" + BuildRequestsJsonLocked() + ",";
    s += "\"pending\":" + BuildPendingJsonLocked() + ",";
    s += "\"chat\":" + BuildChatJsonLocked() + ",";

    if (withFeed) {
        s += "\"feed\":[";
        size_t start = snap.feed.size() > 50 ? snap.feed.size() - 50 : 0;
        bool first = true;
        for (size_t i = start; i < snap.feed.size(); ++i) {
            if (!first) s += ",";
            first = false;
            s += BuildEventJson(snap.feed[i]);
        }
        s += "],";
    }
    s += "\"last_seq\":" + std::to_string(snap.last_seq) + "}";
    return s;
}

std::string App::BuildHealthJson() {
    std::lock_guard<std::mutex> g(mtx);
    std::string s = "{\"ok\":true,\"ts\":" + std::to_string(now_ms());
    s += ",\"version\":\"" + json_escape(version) + "\"";
    s += ",\"in_game\":" + std::string(snap.in_game ? "true" : "false");
    s += ",\"db_size\":" + std::to_string(snap.db_size);
    s += "}";
    return s;
}

} // namespace csk
