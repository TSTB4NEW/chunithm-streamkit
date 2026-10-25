// chunithm-streamkit — state.h
// 全局状态模型：一局游戏内被只读采样的所有数据 + 判定事件队列。
#pragma once
#include <cstdint>
#include <string>
#include <deque>
#include <array>

namespace csk {

enum JudgeType : uint8_t {
    JUDGE_JC = 0,        // JUSTICE CRITICAL
    JUDGE_JU = 1,        // JUSTICE
    JUDGE_AT = 2,        // ATTACK
    JUDGE_MI = 3,        // MISS
    JUDGE_UNKNOWN = 255,
};

inline const char* JudgeName(uint8_t t) {
    switch (t) {
    case JUDGE_JC: return "JUSTICE_CRITICAL";
    case JUDGE_JU: return "JUSTICE";
    case JUDGE_AT: return "ATTACK";
    case JUDGE_MI: return "MISS";
    default: return "UNKNOWN";
    }
}

struct JudgeEvent {
    uint64_t seq = 0;      // 全局递增序号（供 SSE 增量去重）
    uint32_t t_ms = 0;     // 采集时间（GetTickCount）
    uint8_t type = JUDGE_UNKNOWN;
    uint8_t lane = 255;    // 0-31 轨道；255=未知（计数器差值模式）
    int32_t delta_ms = 0;  // 相对判定点偏差（毫秒，可能为负；未知=0）
};

struct SongInfo {
    uint32_t id = 0;
    std::string title;
    std::string artist;
    double bpm = 0.0;
    // 顺序：BASIC/ADVANCED/EXPERT/MASTER/ULTIMA/WORLD'S END
    std::array<std::string, 6> diff_names = {"BASIC", "ADVANCED", "EXPERT",
                                             "MASTER", "ULTIMA", "WORLD'S END"};
    std::array<double, 6> diff_const{};   // 0 表示未知
    std::string we_tag;                   // World's End 标签（止/狂/戻…）
    std::string genre;                    // 曲目分类（原创 / 东方Project / niconico…）
    std::string version;                  // 版本名（AIR / AMAZON / NEW…）
    uint32_t version_code = 0;
    bool known = false;
};

// —— 点歌队列 ——
struct SongRequest {
    uint64_t id = 0;
    uint32_t song_id = 0;
    unsigned uid = 0;      // 点歌人 UID（取消自己的点歌用）
    std::string title;
    std::string artist;
    std::string user;      // 点歌人昵称
    std::string query;     // 原始点歌文本
    int difficulty = -1;   // 0..5；-1=未指定
    bool playing = false;  // 正在游玩该曲（结束时移除）
    uint32_t t_ms = 0;
};

// —— 最近弹幕（叠加层下方滚动显示）——
struct DanmakuMsg {
    std::string user;
    std::string text;
    uint32_t t_ms = 0;
};

struct Snapshot {
    // —— 判定统计（累计）——
    uint64_t jc = 0, ju = 0, at = 0, mi = 0;
    // —— FAST/LATE 时机统计 ——
    uint32_t fast = 0, late = 0;          // NUM_Fast / NUM_Late
    uint32_t fast_jc = 0, late_jc = 0;    // NUM_Fast_JC / NUM_Late_JC
    uint32_t fast_at = 0, late_at = 0;    // NUM_Fast_Attack / NUM_Late_Attack
    // —— 本局进度 ——
    uint32_t score = 0;
    uint32_t max_score = 0;   // 理论最高分（hook 捕获；0=未知）
    uint32_t best_score = 0;  // 当前所选谱面的历史最佳（选歌界面）
    uint32_t combo = 0;
    uint32_t max_combo = 0;
    uint32_t note_total = 0;   // 已知谱面音符数；0=未知（由 max_combo 推断）
    // —— 歌曲 ——
    uint32_t music_id = 0;
    int difficulty = -1;       // 0..5；-1=未知
    bool in_game = false;
    bool demo = false;
    bool profile_ok = false;   // 至少解析出一组可用地址
    std::string status = "idle"; // idle / playing / demo
    // —— 插件自身 ——
    std::string profile_name;
    uint32_t db_size = 0;
    std::string plugin_version;
    std::string hint;          // 面向主播/调试者的提示信息
    // —— 判定事件队列（SSE 增量消费，保留最近 256 条）——
    uint64_t last_seq = 0;
    std::deque<JudgeEvent> feed;
};

} // namespace csk
