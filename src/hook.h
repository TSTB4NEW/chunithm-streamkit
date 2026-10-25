// chunithm-streamkit — hook.h
// 分数写入指令的 inline hook。
// 目标：chusanApp.exe + 0x7B2E5B
//   89 BE FC 02 00 00   mov [esi+0x2FC], edi
// edi = 新分数，[esi+0x2FC] = 分数存储位置。
// hook 后从 Score() 读取最近一次写入的分数。
#pragma once
#include <cstdint>

namespace csk {
namespace hook {

// 把 DllMain 期间缓冲的 hook 日志落盘（工作线程在 IoReady 之后调用）
void FlushEarlyLog();

// 安装 hook；返回是否成功。失败时插件照常运行（只是分数不可用）。
bool Install();

// 是否已安装
bool Installed();

// 最近一次写入的分数（0 = 还没捕获到）
uint32_t Score();

// 离开对局时清零
void Reset();

// —— max_combo hook ——
// chusanApp.exe + 0x7B81F6:  mov [ebx+0xAC], eax
bool InstallCombo();
bool ComboInstalled();
uint32_t Combo();
void ResetCombo();
constexpr uintptr_t kComboWriteRva = 0x7B81F6;

// —— 歌曲 ID / 难度 hook ——
// chusanApp.exe + 0x9B0648:  mov [esi+0x60], eax ; call 0x449FA3（共 8 字节）
bool InstallSong();
bool SongInstalled();
uint32_t SongId();          // 当前歌曲 ID
uint32_t SongDiffEnc();     // 难度编码（0x40000|难度）
uint32_t SongPtr();         // 歌曲对象地址（调试用）
void ResetSong();
constexpr uintptr_t kSongWriteRva = 0x9B0648;
constexpr uintptr_t kSongCallTarget = 0x449FA3;
constexpr uintptr_t kSongBack = 0xDB0650;

// —— 难度 hook ——
// chusanApp.exe + 0x8B76CF:  mov byte [eax+0x2C0], bl（6 字节）
bool InstallDiff();
bool DiffInstalled();
uint32_t Diff();
void ResetDiff();
constexpr uintptr_t kDiffWriteRva = 0x8B76CF;

// hook 目标
constexpr uintptr_t kScoreWriteRva = 0x7B2E5B;

// —— 选歌界面历史最佳分数 hook ——
// chusanApp.exe + 0x6E09D9:  89 91 4C 02 00 00   mov [ecx+0x24C], edx
// edx = 该谱面历史最佳分数（选歌界面写入，进游玩会被清 0）
bool InstallBestScore();
bool BestScoreInstalled();
uint32_t BestScore();
void ResetBestScore();
constexpr uintptr_t kBestScoreWriteRva = 0x6E09D9;

// —— 理论最高分 hook ——
// chusanApp.exe + 0x96A427:  89 81 8C 01 00 00   mov [rcx+0x18C], eax
// eax = 本局理论最高分（剩余音符全 JC 时的最终分数）
bool InstallMaxScore();
bool MaxScoreInstalled();
uint32_t MaxScore();
void ResetMaxScore();
constexpr uintptr_t kMaxScoreWriteRva = 0x96A427;

// —— FAST / LATE 计数 hook ——
// chusanApp.exe + 0x7B2E1D:  mov [edx+0x58], ecx ; mov ecx, esi（共 5 字节）
// 上游：lea edx,[ebx+eax*4] —— 计数器是「按索引的数组」，+0x58 只是元素偏移
// 所有跳转都指向 0x7B2E1D 或之前，因此从该处打 5 字节补丁不会踩到跳转目标
bool InstallFastLate();
bool FastLateInstalled();
uint32_t FlBase();      // 最近一次命中的对象基址（ebx）
uint32_t FlMainBase();  // 主统计对象：地址最低的那个（数组第 0 个元素）

// 已验证的槽位偏移（相对主对象基址）
constexpr uint32_t kFlOffFastAt = 0x68;
constexpr uint32_t kFlOffLateAt = 0x6C;
constexpr uint32_t kFlOffFastJ  = 0x74;
constexpr uint32_t kFlOffLateJ  = 0x78;
constexpr uint32_t kFlOffFastJc = 0x80;
constexpr uint32_t kFlOffLateJc = 0x84;
void ResetFastLate();
constexpr uintptr_t kFlWriteRva = 0x7B2E1D;
constexpr uintptr_t kFlBack = 0x7B2E22;

} // namespace hook
} // namespace csk
