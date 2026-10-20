// chunithm-streamkit — profile.h
// 版本适配层：把 INI 里的特征码/指针链配置解析为可读地址，并按固定周期
// 从游戏内存做只读采样（绝不写内存、不挂钩子）。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include "ini.h"
#include "sigscan.h"
#include "memory.h"
#include "state.h"
#include "log.h"
#include "util.h"
#include "hook.h"

namespace csk {

struct FieldSpec {
    std::string name;
    std::string sig;        // 字节特征码（空 = 不使用）
    std::string resolve;    // none/abs32/rel32/lea32/pointer
    uintptr_t extra = 0;
    size_t match_index = 0;
    uintptr_t deref = 0;
    std::vector<uintptr_t> chain;   // 无 sig 时使用：chain[0]=RVA(或绝对地址)，后续为偏移
    bool abs_base = false;
    // —— 字符串签名定位（Chuni2Api 机制，优先于 sig/chain）——
    std::string string_sig;         // 内存中的字段名字符串，如 NUM_jctirical
    bool string_nul = true;         // 要求字符串以 \0 结尾
    uintptr_t string_off = 0;       // 值地址 = 命中点 + string_off
    uintptr_t marker_off = 0;       // 校验标记偏移（0=不校验）
    std::vector<uint8_t> marker;    // 校验标记字节
    uintptr_t min_addr = 0x50000000;
    bool pick_highest = true;
    std::string type = "i32";       // i8/u8/i16/u16/i32/u32/i64/u64/f32
    std::string pattern;            // 结构特征扫描：pair1f / song / song_diff
    uint32_t shift = 0;             // 读到的值右移位数
    bool use_hook = false;          // 从 hook 读取（score）
    uintptr_t addr = 0;             // 解析结果（取值地址）
    bool ok = false;
};

struct RingSpec {
    bool enabled = false;
    FieldSpec base;
    uintptr_t record_size = 0;
    uintptr_t count = 0;
    uintptr_t head_off = 0;   // 相对 base 的头索引（u32）
    bool use_seq = false;
    uintptr_t seq_off = 0;    // 相对 base 的序号（u32/u64）
    uintptr_t type_off = 0, lane_off = 0, delta_off = 0;
    bool delta_is_float = true;
    bool ok = false;
};

class Profile {
public:
    bool Load(const Ini& ini, const std::string& sectionName) {
        const Ini::Section* sigs = ini.GetSection("sigs");
        auto loadField = [&](const Ini::Section& sec, const std::string& key, FieldSpec& f) {
            f.name = key;
            f.sig = sec.count(key + ".sig") ? sec.at(key + ".sig") : "";
            if (!f.sig.empty() && sigs && sigs->count(f.sig)) f.sig = sigs->at(f.sig);
            f.resolve = sec.count(key + ".resolve") ? sec.at(key + ".resolve") : "none";
            f.extra = parse_hex(sec.count(key + ".extra") ? sec.at(key + ".extra") : "");
            f.match_index = (size_t)parse_u64(sec.count(key + ".match_index") ? sec.at(key + ".match_index") : "");
            f.deref = parse_hex(sec.count(key + ".deref") ? sec.at(key + ".deref") : "");
            std::string ch = sec.count(key + ".chain") ? sec.at(key + ".chain") : "";
            f.chain.clear();
            if (!ch.empty()) {
                for (auto& p : split(ch, ',')) {
                    std::string t = trim(p);
                    if (t.empty()) continue;
                    if (t[0] == '!') {  // '!' 前缀表示链首为绝对地址
                        f.abs_base = true;
                        f.chain.push_back(parse_hex(t.substr(1)));
                    } else {
                        f.chain.push_back(parse_hex(t));
                    }
                }
            }
            if (sec.count(key + ".type")) f.type = sec.at(key + ".type");
            // 字符串签名
            f.string_sig = sec.count(key + ".string_sig") ? sec.at(key + ".string_sig") : "";
            f.string_nul = parse_i32(sec.count(key + ".string_nul") ? sec.at(key + ".string_nul") : "1") != 0;
            f.string_off = parse_hex(sec.count(key + ".string_off") ? sec.at(key + ".string_off") : "");
            f.marker_off = parse_hex(sec.count(key + ".marker_off") ? sec.at(key + ".marker_off") : "");
            f.marker.clear();
            std::string mk = sec.count(key + ".marker") ? sec.at(key + ".marker") : "";
            if (!mk.empty()) {
                for (auto& p : split(mk, ' ')) {
                    if (!trim(p).empty()) f.marker.push_back((uint8_t)parse_hex(p));
                }
            }
            f.min_addr = sec.count(key + ".min_addr") ? parse_hex(sec.at(key + ".min_addr")) : 0x50000000;
            f.pick_highest = parse_i32(sec.count(key + ".pick_highest") ? sec.at(key + ".pick_highest") : "1") != 0;
            f.pattern = sec.count(key + ".pattern") ? sec.at(key + ".pattern") : "";
            f.shift = (uint32_t)parse_u64(sec.count(key + ".shift") ? sec.at(key + ".shift") : "0");
            f.use_hook = parse_i32(sec.count(key + ".hook") ? sec.at(key + ".hook") : "0") != 0;
        };

        const Ini::Section* sec = ini.GetSection(sectionName);
        if (!sec) return false;
        profile_name_ = sectionName;

        loadField(*sec, "music_id", fields_[F_MUSIC_ID]);
        loadField(*sec, "difficulty", fields_[F_DIFFICULTY]);
        loadField(*sec, "score", fields_[F_SCORE]);
        loadField(*sec, "combo", fields_[F_COMBO]);
        loadField(*sec, "max_combo", fields_[F_MAX_COMBO]);
        loadField(*sec, "justice_crit", fields_[F_JC]);
        loadField(*sec, "justice", fields_[F_JU]);
        loadField(*sec, "attack", fields_[F_AT]);
        loadField(*sec, "miss", fields_[F_MI]);
        loadField(*sec, "fast", fields_[F_FAST]);
        loadField(*sec, "late", fields_[F_LATE]);
        loadField(*sec, "fast_jc", fields_[F_FAST_JC]);
        loadField(*sec, "late_jc", fields_[F_LATE_JC]);
        loadField(*sec, "fast_attack", fields_[F_FAST_AT]);
        loadField(*sec, "late_attack", fields_[F_LATE_AT]);
        loadField(*sec, "note_total", fields_[F_NOTE_TOTAL]);

        // 判定事件环形缓冲（可选；不配置时回退到计数器差值）
        const Ini::Section* ring = ini.GetSection(sectionName + ".ring");
        if (ring) {
            ring_.enabled = parse_i32(ring->count("enabled") ? ring->at("enabled") : "0") != 0;
            if (ring_.enabled) {
                loadField(*ring, "base", ring_.base);
                ring_.record_size = parse_hex(ring->count("record_size") ? ring->at("record_size") : "");
                ring_.count = parse_hex(ring->count("count") ? ring->at("count") : "");
                ring_.head_off = parse_hex(ring->count("head_off") ? ring->at("head_off") : "");
                ring_.use_seq = parse_i32(ring->count("use_seq") ? ring->at("use_seq") : "0") != 0;
                ring_.seq_off = parse_hex(ring->count("seq_off") ? ring->at("seq_off") : "");
                ring_.type_off = parse_hex(ring->count("type_off") ? ring->at("type_off") : "");
                ring_.lane_off = parse_hex(ring->count("lane_off") ? ring->at("lane_off") : "");
                ring_.delta_off = parse_hex(ring->count("delta_off") ? ring->at("delta_off") : "");
                ring_.delta_is_float = parse_i32(ring->count("delta_is_float") ? ring->at("delta_is_float") : "1") != 0;
            }
        }
        event_source_ = ini.Get(sectionName, "event_source", "auto");
        return true;
    }

    // 解析所有地址；返回成功解析的字段数
    size_t Resolve(ModuleScanner& scanner) {
        resolved_ = 0;
        for (auto& f : fields_) {
            f.addr = 0;
            f.ok = false;
            if (!f.pattern.empty()) continue;   // 交给 ResolvePatterns() 扫描
            if (!f.string_sig.empty()) {
                StringSigSpec spec;
                spec.text = f.string_sig;
                spec.require_nul = f.string_nul;
                spec.value_offset = f.string_off;
                spec.marker_offset = f.marker_off;
                spec.marker = f.marker;
                spec.min_addr = f.min_addr;
                spec.pick_highest = f.pick_highest;
                uintptr_t addr = 0;
                f.ok = ScanStringHeap(spec, addr) && addr != 0;
                f.addr = addr;
            } else if (!f.sig.empty()) {
                f.ok = scanner.ResolveBySig(f.sig, f.resolve, f.extra, f.match_index, f.deref, f.addr);
            } else if (!f.chain.empty()) {
                uintptr_t base = f.abs_base ? 0 : scanner.base();
                f.ok = ResolveChain(base, f.chain, f.addr);
            }
            if (f.ok && f.addr) {
                ++resolved_;
                L().Debug("profile: %s -> %p", f.name.c_str(), (void*)f.addr);
            } else {
                L().Debug("profile: %s NOT resolved (str_sig=%s sig=%s chain=%zu)",
                          f.name.c_str(),
                          f.string_sig.empty() ? "-" : f.string_sig.c_str(),
                          f.sig.empty() ? "-" : f.sig.c_str(), f.chain.size());
            }
        }
        // 记住判定字符串基址，供结构特征扫描（score 等）做窗口锚点
        if (fields_[F_JC].ok) {
            judge_anchor_ = fields_[F_JC].addr - fields_[F_JC].string_off;
            ResolvePatterns();
        }
        ring_.ok = false;
        if (ring_.enabled) {
            if (!ring_.base.string_sig.empty()) {
                StringSigSpec spec;
                spec.text = ring_.base.string_sig;
                spec.require_nul = ring_.base.string_nul;
                spec.value_offset = ring_.base.string_off;
                spec.marker_offset = ring_.base.marker_off;
                spec.marker = ring_.base.marker;
                spec.min_addr = ring_.base.min_addr;
                spec.pick_highest = ring_.base.pick_highest;
                uintptr_t addr = 0;
                ring_.ok = ScanStringHeap(spec, addr) && addr != 0;
                ring_.base.addr = addr;
            } else if (!ring_.base.sig.empty()) {
                ring_.ok = scanner.ResolveBySig(ring_.base.sig, ring_.base.resolve,
                                                ring_.base.extra, ring_.base.match_index,
                                                ring_.base.deref, ring_.base.addr);
            } else if (!ring_.base.chain.empty()) {
                uintptr_t b = ring_.base.abs_base ? 0 : scanner.base();
                ring_.ok = ResolveChain(b, ring_.base.chain, ring_.base.addr);
            }
            if (ring_.ok && ring_.base.addr && ring_.record_size > 0 && ring_.count > 0) {
                L().Info("judge ring resolved @ %p, record=%zx count=%zx",
                         (void*)ring_.base.addr, ring_.record_size, ring_.count);
            } else {
                ring_.ok = false;
                L().Info("judge ring disabled/unresolved, falling back to counter diff");
            }
        }
        return resolved_;
    }

    bool AnyOk() const { return resolved_ > 0; }

    // 扫描当前难度（结构布局扫描，取地址最低的一份）
    void RefreshDifficulty() {
        uint32_t d = 0, mid = 0;
        if (ScanCurrentDiff(d, mid)) {
            scan_diff_ = d;
            scan_diff_valid_ = true;
        } else {
            scan_diff_valid_ = false;
        }
    }

    // 重新扫描「当前选中歌曲」结构（换歌后地址会变）
    void RescanSong() {        bool needSong = false;
        for (auto& f : fields_)
            if (f.pattern == "song" || f.pattern == "song_diff") needSong = true;
        if (!needSong) return;
        uintptr_t a = 0;
        uint32_t mid = 0, enc = 0;
        if (!ScanSongStruct(a, mid, enc)) {
            // 扫不到（例如对局中该结构不存在）→ 清空，避免读到旧地址的垃圾值
            song_addr_ = 0;
            for (auto& f : fields_) {
                if (f.pattern == "song" || f.pattern == "song_diff") {
                    f.addr = 0;
                    f.ok = false;
                }
            }
            return;
        }
        song_addr_ = a;
        for (auto& f : fields_) {
            if (f.pattern == "song") { f.addr = a + 4; f.ok = true; }
            else if (f.pattern == "song_diff") { f.addr = a; f.ok = true; }
        }
    }

    // 四个判定字段是否全部解析成功（用于决定是否继续重扫）
    // 四项判定解析成功即视为完成
    // （FAST/LATE 已改由 inline hook 读取，不再依赖字符串签名）
    bool JudgesOk() const {
        return fields_[F_JC].ok && fields_[F_JU].ok &&
               fields_[F_AT].ok && fields_[F_MI].ok;
    }

    // 主采样：只读游戏内存并推进事件队列
    void Sample(Snapshot& snap) {
        snap.profile_ok = AnyOk();
        snap.status = "idle";

        uint64_t jc = 0, ju = 0, at = 0, mi = 0;
        uint32_t musicId = 0, score = 0, combo = 0, maxCombo = 0, noteTotal = 0;
        int difficulty = -1;

        if (fields_[F_MUSIC_ID].use_hook && hook::SongInstalled()) musicId = hook::SongId();
        else ReadField(fields_[F_MUSIC_ID], musicId);
        if (scan_diff_valid_) {
            difficulty = (int)scan_diff_;
        } else if (fields_[F_DIFFICULTY].use_hook && hook::DiffInstalled()) {
            int d = (int)hook::Diff();
            difficulty = (d <= 5) ? d : -1;
        } else if (fields_[F_DIFFICULTY].use_hook && hook::SongInstalled()) {
            int d = (int)(hook::SongDiffEnc() & 0xFFFF);
            difficulty = (d <= 5) ? d : -1;
        } else {
            ReadField(fields_[F_DIFFICULTY], difficulty);
        }
        if (fields_[F_SCORE].use_hook && hook::Installed()) score = hook::Score();
        else ReadField(fields_[F_SCORE], score);
        if (fields_[F_COMBO].use_hook && hook::ComboInstalled()) combo = hook::Combo();
        else ReadField(fields_[F_COMBO], combo);
        if (fields_[F_MAX_COMBO].use_hook && hook::ComboInstalled()) maxCombo = hook::Combo();
        else ReadField(fields_[F_MAX_COMBO], maxCombo);
        ReadField(fields_[F_NOTE_TOTAL], noteTotal);
        ReadField(fields_[F_JC], jc);
        ReadField(fields_[F_JU], ju);
        ReadField(fields_[F_AT], at);
        ReadField(fields_[F_MI], mi);

        uint32_t fastV = 0, lateV = 0, fastJC = 0, lateJC = 0, fastAT = 0, lateAT = 0;
        // FAST/LATE 来自 hook 捕获的主统计对象（偏移已验证）
        if (hook::FastLateInstalled()) {
            uint32_t fb = hook::FlMainBase();
            if (fb) {
                auto rdOff = [&](uint32_t off) -> uint32_t {
                    uint32_t v = 0;
                    if (!SafeRead((uintptr_t)fb + off, &v, sizeof(v))) return 0;
                    return v > 100000 ? 0 : v;
                };
                fastAT = rdOff(hook::kFlOffFastAt);
                lateAT = rdOff(hook::kFlOffLateAt);
                fastJC = rdOff(hook::kFlOffFastJc);
                lateJC = rdOff(hook::kFlOffLateJc);
                uint32_t fj = rdOff(hook::kFlOffFastJ);
                uint32_t lj = rdOff(hook::kFlOffLateJ);
                fastV = fastJC + fj + fastAT;
                lateV = lateJC + lj + lateAT;
                // 合理性校验：早+晚不可能超过已判定总数，否则视为脏数据
                uint64_t judged = jc + ju + at;
                if (judged == 0 || (uint64_t)fastV + lateV > judged + 8) {
                    fastV = lateV = fastJC = lateJC = fastAT = lateAT = 0;
                }
            }
        }
        ReadField(fields_[F_FAST], fastV);
        ReadField(fields_[F_LATE], lateV);
        ReadField(fields_[F_FAST_JC], fastJC);
        ReadField(fields_[F_LATE_JC], lateJC);
        ReadField(fields_[F_FAST_AT], fastAT);
        ReadField(fields_[F_LATE_AT], lateAT);

        // 未初始化内存的垃圾值防护（Chuni2Api 同款：>30000 视为 0）
        auto clean = [](uint64_t v) -> uint64_t { return v > 30000 ? 0 : v; };
        jc = clean(jc); ju = clean(ju); at = clean(at); mi = clean(mi);

        // 对局判定只看四项判定计数：结算退出后计数归零 → 触发清零。
        // （不能带 musicId：歌曲 hook 捕获的值在选歌界面一直有效，会让判定永远为真）
        bool judgePlaying = (jc + ju + at + mi > 0);
        if (!judgePlaying) {
            // 离开对局：清基线 + 清 hook 捕获值，防止下一局误报/残留
            had_game_ = false;
            last_ = CounterSet{};
            combo_ = 0;
            combo_max_ = 0;
            ring_head_valid_ = false;
            if (fields_[F_SCORE].use_hook) hook::Reset();
            if (fields_[F_COMBO].use_hook || fields_[F_MAX_COMBO].use_hook) hook::ResetCombo();
            if (fields_[F_SCORE].use_hook) score = hook::Score();
            if (fields_[F_COMBO].use_hook || fields_[F_MAX_COMBO].use_hook) {
                combo = hook::Combo();
                maxCombo = hook::Combo();
            }
        }
        bool inGame = judgePlaying || score > 0;
        // 事件来源决策：ring 模式（且 ring 可用）时不发计数器差值，避免重复
        bool useRingEvents = ring_.ok && event_source_ != "counter";
        if (inGame && judgePlaying) {
            if (!had_game_) {
                had_game_ = true;
                last_ = CounterSet{jc, ju, at, mi};
                ring_head_valid_ = false;
            }
            if (!useRingEvents) {
                EmitCounterDiffs(jc, ju, at, mi, snap);
                last_ = CounterSet{jc, ju, at, mi};
            }
        }

        snap.jc = jc; snap.ju = ju; snap.at = at; snap.mi = mi;
        snap.fast = fastV; snap.late = lateV;
        snap.fast_jc = fastJC; snap.late_jc = lateJC;
        snap.fast_at = fastAT; snap.late_at = lateAT;
        snap.score = score;
        snap.combo = combo_;
        snap.max_combo = (maxCombo > combo_max_) ? maxCombo : combo_max_;
        snap.note_total = noteTotal;
        snap.music_id = musicId;
        snap.difficulty = difficulty;
        snap.in_game = inGame;
        if (inGame) snap.status = "playing";

        // 环形缓冲事件（ring 或 auto+ring 可用时）
        if (useRingEvents) SampleRing(snap);
    }

    // 供 SSE 分配全局事件序号
    uint64_t AllocSeq() { return ++next_seq_; }

    const std::string& profile_name() const { return profile_name_; }
    std::string Describe() const {
        std::string s = "resolved=" + std::to_string(resolved_);
        s += " ring=" + std::string(ring_.ok ? "yes" : "no");
        return s;
    }

private:
    enum FieldId {
        F_MUSIC_ID = 0, F_DIFFICULTY, F_SCORE, F_COMBO, F_MAX_COMBO,
        F_JC, F_JU, F_AT, F_MI, F_NOTE_TOTAL,
        F_FAST, F_LATE, F_FAST_JC, F_LATE_JC, F_FAST_AT, F_LATE_AT,
        F_COUNT
    };

    struct CounterSet {
        uint64_t jc = 0, ju = 0, at = 0, mi = 0;
    };

    // 结构特征扫描：曲目结构 / score 记录
    void ResolvePatterns() {
        bool needSong = false;
        for (auto& f : fields_)
            if (f.pattern == "song" || f.pattern == "song_diff") needSong = true;
        if (needSong) {
            uintptr_t a = 0;
            uint32_t mid = 0, enc = 0;
            if (ScanSongStruct(a, mid, enc)) {
                song_addr_ = a;
                L().Debug("profile: song struct -> %p (id=%u enc=0x%X)", (void*)a, mid, enc);
            }
        }
        for (auto& f : fields_) {
            if (f.pattern.empty()) continue;
            if (f.pattern == "song") {
                if (song_addr_) { f.addr = song_addr_ + 4; f.ok = true; }   // +0x04 = 曲目ID
            } else if (f.pattern == "song_diff") {
                if (song_addr_) { f.addr = song_addr_; f.ok = true; }       // +0x00 = 难度
            } else {
                uintptr_t addr = 0;
                uint32_t val = 0;
                if (ScanPair1F(judge_anchor_, 0x8000, addr, val)) {
                    f.addr = addr;
                    f.ok = true;
                    L().Debug("profile: %s pattern=%s -> %p (val=%u)",
                              f.name.c_str(), f.pattern.c_str(), (void*)addr, val);
                }
            }
        }
    }

    uintptr_t judge_anchor_ = 0;
    uintptr_t song_addr_ = 0;

    template <typename T>
    void ReadField(const FieldSpec& f, T& out) {
        if (!f.ok) return;
        if (f.type == "f32") {
            float v = 0; SafeReadT(f.addr, v); out = (T)v; return;
        }
        uint64_t v = 0;
        if (f.type == "u8") {
            uint8_t x = 0; SafeReadT(f.addr, x); v = x;
        } else if (f.type == "i8") {
            int8_t x = 0; SafeReadT(f.addr, x); v = (uint64_t)(int64_t)x;
        } else if (f.type == "u16") {
            uint16_t x = 0; SafeReadT(f.addr, x); v = x;
        } else if (f.type == "i16") {
            int16_t x = 0; SafeReadT(f.addr, x); v = (uint64_t)(int64_t)x;
        } else if (f.type == "u32") {
            uint32_t x = 0; SafeReadT(f.addr, x); v = x;
        } else if (f.type == "u64") {
            uint64_t x = 0; SafeReadT(f.addr, x); v = x;
        } else if (f.type == "i64") {
            int64_t x = 0; SafeReadT(f.addr, x); v = (uint64_t)x;
        } else {
            int32_t x = 0; SafeReadT(f.addr, x); v = (uint64_t)(int64_t)x;
        }
        if (f.shift) v >>= f.shift;
        out = (T)v;
    }

    void PushEvent(Snapshot& snap, uint8_t type, uint8_t lane, int32_t deltaMs) {
        JudgeEvent ev;
        ev.seq = AllocSeq();
        ev.t_ms = now_ms();
        ev.type = type;
        ev.lane = lane;
        ev.delta_ms = deltaMs;
        snap.feed.push_back(ev);
        snap.last_seq = ev.seq;
        while (snap.feed.size() > 256) snap.feed.pop_front();
    }

    // 计数器差值 → 判定事件（无法区分轨道，lane=255）
    void EmitCounterDiffs(uint64_t jc, uint64_t ju, uint64_t at, uint64_t mi, Snapshot& snap) {
        auto emit = [&](uint64_t now, uint64_t prev, uint8_t type) {
            if (now > prev) {
                uint64_t d = now - prev;
                if (d > 32) d = 32;   // 防首次/断线洪泛
                for (uint64_t i = 0; i < d; ++i) PushEvent(snap, type, 255, 0);
            }
        };
        emit(jc, last_.jc, JUDGE_JC);
        emit(ju, last_.ju, JUDGE_JU);
        emit(at, last_.at, JUDGE_AT);
        emit(mi, last_.mi, JUDGE_MI);

        // —— combo 推断：命中判定增加则累加，MISS 增加则归零 ——
        uint64_t dh = 0;
        if (jc > last_.jc) dh += jc - last_.jc;
        if (ju > last_.ju) dh += ju - last_.ju;
        if (at > last_.at) dh += at - last_.at;
        uint64_t dm = (mi > last_.mi) ? (mi - last_.mi) : 0;
        if (dm > 0) {
            combo_ = 0;
        } else if (dh > 0) {
            if (dh > 32) dh = 32;
            combo_ += (uint32_t)dh;
        }
        if (combo_ > combo_max_) combo_max_ = combo_;
    }

    // 环形缓冲读取：head 索引（或 seq 序号）前进多少就取多少条记录
    void SampleRing(Snapshot& snap) {
        uintptr_t base = ring_.base.addr;
        if (!base || ring_.count == 0 || ring_.record_size == 0) return;

        uint32_t head = 0;
        if (!SafeReadT(base + ring_.head_off, head)) return;
        head %= (uint32_t)ring_.count;

        uint32_t last = head;
        if (ring_head_valid_ && last_head_ != head) {
            uint32_t n = (head - last_head_ + (uint32_t)ring_.count) % (uint32_t)ring_.count;
            if (n > ring_.count / 2 && n > 0) n = 1; // 异常大跳变：保守只取最新
            if (n > 64) n = 64;
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t idx = (last_head_ + 1 + i) % (uint32_t)ring_.count;
                uintptr_t rec = base + (uintptr_t)idx * ring_.record_size;
                uint32_t type = JUDGE_UNKNOWN;
                uint32_t lane = 255;
                int32_t delta = 0;
                if (ring_.type_off || !SafeReadT(rec + ring_.type_off, type)) type = JUDGE_UNKNOWN;
                if (ring_.lane_off) SafeReadT(rec + ring_.lane_off, lane);
                if (ring_.delta_off) {
                    if (ring_.delta_is_float) {
                        float f = 0; SafeReadT(rec + ring_.delta_off, f); delta = (int32_t)f;
                    } else {
                        SafeReadT(rec + ring_.delta_off, delta);
                    }
                }
                if (type <= JUDGE_MI) PushEvent(snap, (uint8_t)type, (uint8_t)lane, delta);
            }
        }
        last_head_ = last;
        ring_head_valid_ = true;
        // 忽略 seq 偏移（head 方式已够）；use_seq 留作扩展
    }

    std::array<FieldSpec, F_COUNT> fields_{};
    RingSpec ring_;
    std::string event_source_ = "auto";
    std::string profile_name_;
    size_t resolved_ = 0;
    uint64_t next_seq_ = 0;
    CounterSet last_;
    uint32_t combo_ = 0;       // 由判定差值推断的当前 combo
    uint32_t combo_max_ = 0;   // 推断出的最大 combo
    uint32_t scan_diff_ = 0;   // 结构扫描得到的当前难度
    bool scan_diff_valid_ = false;
    bool had_game_ = false;
    uint32_t last_head_ = 0;
    bool ring_head_valid_ = false;
};

} // namespace csk
