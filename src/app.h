// chunithm-streamkit — app.h
// 进程内单例：受互斥锁保护的全局快照 + 歌曲库 + 状态 JSON 构建。
#pragma once
#include <mutex>
#include <atomic>
#include <string>
#include <unordered_map>
#include "state.h"
#include "songdb.h"
#include "profile.h"

namespace csk {

class App {
public:
    static App& Get() { static App inst; return inst; }

    std::mutex mtx;
    Snapshot snap;
    SongDb db;
    Profile profile;
    std::string version = "1.0.6";
    std::string web_root;
    std::string config_path;
    bool demo = false;
    // 演示模式使用的临时歌曲表（与 db 同锁保护）
    std::unordered_map<uint32_t, SongInfo> demoSongs;

    // —— 点歌队列（独立于 snap，避免主采样循环每帧覆盖）——
    struct RequestNotice {
        std::string user;
        std::string query;
        std::string title;
        std::string status;
        std::string text;     // 叠加层直接显示的文案
        uint32_t t_ms = 0;
    };

    // 模糊匹配后等待观众选择
    struct RequestCandidate {
        uint32_t song_id = 0;
        std::string title;
        std::string artist;
        std::array<double, 6> consts{};
        std::string we_tag;
    };
    struct PendingSelection {
        bool active = false;
        unsigned uid = 0;
        std::string user;
        std::string query;
        int difficulty = -1;
        uint32_t t_ms = 0;
        std::vector<RequestCandidate> candidates;
    };

    std::atomic<bool> request_enabled{false};
    int request_max = 12;
    unsigned owner_uid = 0;   // 房主 UID（只有房主能删歌/清空）
    std::string request_path; // songrequest.tsv 路径
    uint64_t request_seq = 0;
    std::deque<SongRequest> requests;   // 受 mtx 保护
    RequestNotice request_last;
    PendingSelection pending;           // 受 mtx 保护

    // 最近弹幕（最多 5 条，叠加层下方显示）
    std::deque<DanmakuMsg> chat;        // 受 mtx 保护
    void AddDanmaku(const std::string& user, const std::string& text);

    // 配置点歌开关与队列上限（启动时调用）
    void ConfigureRequests(bool enabled, int maxQueue);
    // 运行时开关点歌（不重启）；会取消待选择状态，但不删已有队列
    void SetRequestsEnabled(bool enabled);
    bool RequestsEnabled() const { return request_enabled.load(); }
    // 队列持久化：songrequest.tsv（数据目录）
    void SetQueuePath(const std::string& path);
    void LoadQueue();
    // 收到一条点歌弹幕；返回状态字符串
    std::string AddSongRequest(const std::string& user, unsigned uid,
                               const std::string& query);
    // 随机点歌：按难度/定数/分类/版本筛选；无条件则全范围随机且不指定难度
    std::string AddRandomRequest(const std::string& user, unsigned uid,
                                 const std::string& args);
    // 观众回复数字完成选择；返回是否已消费该消息
    bool HandleSelection(unsigned uid, int number);
    // 定时清理超时的待选择状态
    void ExpireSelection();
    // 观众取消自己的点歌；返回移除数量
    int CancelRequestsByUser(unsigned uid, const std::string& user);
    // 房主按「序号 / 曲名」删歌；返回状态字符串
    std::string OwnerRemoveRequest(const std::string& arg);
    bool RemoveRequest(uint64_t id);
    bool MoveRequest(uint64_t id, int toIndex);
    bool MovePendingCandidate(int from, int to);
    bool RemovePendingCandidate(int index);
    bool ChoosePendingCandidate(int index);
    void ClearRequests();
    // 给叠加层显示一条提示（如权限不足）
    void SetNotice(const std::string& status, const std::string& text);
    // 调用处须持有 mtx
    void SaveQueueLocked();
    void SetNoticeLocked(const std::string& status, const std::string& text);
    void CancelPendingLocked();
    std::string QueueCandidateLocked(const RequestCandidate& c, const std::string& user,
                                     unsigned uid, const std::string& query, int diff,
                                     uint32_t t);
    std::string BuildPendingJsonLocked();
    // 游戏中开始某首歌时标记为「正在游玩」；回到选歌状态时调用 OnSongEnded 移除
    void OnSongStarted(uint32_t musicId);
    void OnSongEnded();

    // 构建 /state 返回的 JSON（内部自行加锁）
    std::string BuildStateJson(bool withFeed);
    std::string BuildEventJson(const JudgeEvent& ev);
    std::string BuildHealthJson();
    std::string BuildRequestsJson();
    std::string BuildChatJson();
    // 调用处须持有 mtx
    std::string BuildRequestsJsonLocked();
    std::string BuildChatJsonLocked();
};

} // namespace csk
