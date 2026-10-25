// chunithm-streamkit — hook.cpp
// 在分数写入指令处安装 inline hook，捕获 edi（新分数）。
#include "hook.h"
#include "log.h"
#include "util.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>

namespace csk {
namespace hook {

// 早期安全日志（DllMain 里 Logger 尚未初始化，直接写文件）
static std::vector<std::string>& EarlyHookLines() {
    static std::vector<std::string> v;
    return v;
}

// DllMain 里也会走到这里（早期分数 hook）：
// capnhook 的 iohook 初始化前拦截文件 API 会断言，所以先缓冲。
static void HookLog(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (!csk::IoReady()) {
        EarlyHookLines().push_back(line);
        return;
    }
    char exeBuf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exeBuf, sizeof(exeBuf));
    std::string p = exeBuf;
    size_t pos = p.find_last_of("\\/");
    if (pos != std::string::npos) p = p.substr(0, pos);
    p = DataDirOf(p) + "\\logs\\csk_hook.log";
    FILE* f = fopen(p.c_str(), "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

void FlushEarlyLog() {
    if (EarlyHookLines().empty()) return;
    char exeBuf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exeBuf, sizeof(exeBuf));
    std::string p = exeBuf;
    size_t pos = p.find_last_of("\\/");
    if (pos != std::string::npos) p = p.substr(0, pos);
    p = DataDirOf(p) + "\\logs\\csk_hook.log";
    FILE* f = fopen(p.c_str(), "a");
    if (!f) return;
    for (auto& l : EarlyHookLines()) fprintf(f, "%s\n", l.c_str());
    fclose(f);
    EarlyHookLines().clear();
}

// 原始指令：mov [esi+0x2FC], edi
static const uint8_t kOrig[6] = {0x89, 0xBE, 0xFC, 0x02, 0x00, 0x00};
// max_combo 原始指令：mov [ebx+0xAC], eax
static const uint8_t kOrigCombo[6] = {0x89, 0x83, 0xAC, 0x00, 0x00, 0x00};
// 歌曲原始指令：mov [esi+0x60], eax ; call 0x449FA4（共 8 字节）
static const uint8_t kOrigSong[8] = {0x89, 0x46, 0x60, 0xE8, 0x53, 0x99, 0x69, 0xFF};
// 难度原始指令：mov byte [eax+0x2C0], bl（6 字节）
static const uint8_t kOrigDiff[6] = {0x88, 0x98, 0xC0, 0x02, 0x00, 0x00};

// 理论最高分原始指令：mov [rcx+0x18C], eax（6 字节）
static const uint8_t kOrigMaxScore[6] = {0x89, 0x81, 0x8C, 0x01, 0x00, 0x00};

// 历史最佳分数原始指令：mov [ecx+0x24C], edx（6 字节）
static const uint8_t kOrigBestScore[6] = {0x89, 0x91, 0x4C, 0x02, 0x00, 0x00};

static volatile uint32_t g_score = 0;
static volatile uint32_t g_combo = 0;
static volatile uint32_t g_song_id = 0;
static volatile uint32_t g_song_ptr = 0;
static bool g_installed = false;
static bool g_combo_installed = false;
static bool g_song_installed = false;
static volatile uint32_t g_diff = 0;
static bool g_diff_installed = false;
static bool g_fl_installed = false;
static bool g_maxscore_installed = false;
static bool g_bestscore_installed = false;
static volatile uint32_t g_best_score = 0;
static volatile uint32_t g_max_score = 0;

extern "C" {
// 供汇编 stub 读写
volatile uint32_t csk_hook_score = 0;
void* csk_hook_back = nullptr;
volatile uint32_t csk_hook_combo = 0;
void* csk_hook_combo_back = nullptr;
volatile uint32_t csk_hook_song_id = 0;
volatile uint32_t csk_hook_song_ptr = 0;
void* csk_hook_song_target = nullptr;
void* csk_hook_song_back = nullptr;
volatile uint32_t csk_hook_best_score = 0;
void* csk_hook_best_score_back = nullptr;
volatile uint32_t csk_hook_max_score = 0;
void* csk_hook_max_score_back = nullptr;
volatile uint32_t csk_hook_diff = 0;
void* csk_hook_diff_back = nullptr;
volatile uint32_t csk_hook_fl_main = 0;
volatile uint32_t csk_hook_fl_base = 0;
volatile uint32_t csk_hook_fl_val = 0;
void* csk_hook_fl_back = nullptr;
}

// 汇编 stub：保存现场 → 记录 edi → 还原 → 执行原指令 → 跳回
extern "C" __attribute__((naked)) void csk_score_stub() {
    __asm__ __volatile__(
        "pushal\n\t"
        "movl %edi, csk_hook_score\n\t"
        "popal\n\t"
        "movl %edi, 0x2fc(%esi)\n\t"
        "jmp *csk_hook_back\n\t"
    );
}

// max_combo stub：保存现场 → 记录 eax → 还原 → 执行原指令 → 跳回
extern "C" __attribute__((naked)) void csk_combo_stub() {
    __asm__ __volatile__(
        "pushal\n\t"
        "movl %eax, csk_hook_combo\n\t"
        "popal\n\t"
        "movl %eax, 0xac(%ebx)\n\t"
        "jmp *csk_hook_combo_back\n\t"
    );
}

// 歌曲 stub：捕获 eax(ID) 与 esi(对象)，执行原指令后重建 call，再跳回
extern "C" __attribute__((naked)) void csk_song_stub() {
    __asm__ __volatile__(
        "pushal\n\t"
        "movl %eax, csk_hook_song_id\n\t"
        "movl %esi, csk_hook_song_ptr\n\t"
        "popal\n\t"
        "movl %eax, 0x60(%esi)\n\t"
        "call *csk_hook_song_target\n\t"
        "jmp *csk_hook_song_back\n\t"
    );
}

// 难度 stub：捕获 bl，执行原字节写入后跳回
extern "C" __attribute__((naked)) void csk_diff_stub() {
    __asm__ __volatile__(
        "pushal\n\t"
        "movzbl %bl, %eax\n\t"
        "movl %eax, csk_hook_diff\n\t"
        "popal\n\t"
        "movb %bl, 0x2c0(%eax)\n\t"
        "jmp *csk_hook_diff_back\n\t"
    );
}

// FAST/LATE stub：维护「最小基址」= 主统计对象（数组第 0 个元素），
// 然后重放两条原指令：mov [edx+0x58],ecx ; mov ecx,esi
extern "C" __attribute__((naked)) void csk_fl_stub() {
    __asm__ __volatile__(
        "pushal\n\t"
        "movl csk_hook_fl_main, %eax\n\t"
        "testl %eax, %eax\n\t"
        "jz 1f\n\t"
        "cmpl %eax, %ebx\n\t"
        "jae 2f\n\t"
        "1:\n\t"
        "movl %ebx, csk_hook_fl_main\n\t"
        "2:\n\t"
        "movl %ebx, csk_hook_fl_base\n\t"
        "movl %ecx, csk_hook_fl_val\n\t"
        "popal\n\t"
        "movl %ecx, 0x58(%edx)\n\t"
        "movl %esi, %ecx\n\t"
        "jmp *csk_hook_fl_back\n\t"
    );
}

// 理论最高分 stub：捕获 eax，重放 mov [rcx+0x18C], eax 后跳回
extern "C" __attribute__((naked)) void csk_maxscore_stub() {
    __asm__ __volatile__(
        "pushal\n\t"
        "movl %eax, csk_hook_max_score\n\t"
        "popal\n\t"
        "movl %eax, 0x18c(%ecx)\n\t"
        "jmp *csk_hook_max_score_back\n\t"
    );
}

extern "C" __attribute__((naked)) void csk_bestscore_stub() {
    __asm__ __volatile__(
        "pushal\n\t"
        "movl %edx, csk_hook_best_score\n\t"
        "popal\n\t"
        "movl %edx, 0x24c(%ecx)\n\t"
        "jmp *csk_hook_best_score_back\n\t"
    );
}

static bool InstallAt(uintptr_t rva, const uint8_t* orig, size_t n,
                      void* stub, volatile uint32_t* outVal, void** back) {
    HMODULE base = GetModuleHandleA(nullptr);
    if (!base) return false;
    uintptr_t target = (uintptr_t)base + rva;
    if (memcmp((const void*)target, orig, n) != 0) {
        char buf[160];
        const uint8_t* p = (const uint8_t*)target;
        snprintf(buf, sizeof(buf),
                 "hook: bytes mismatch at %p: %02X %02X %02X %02X %02X %02X",
                 (void*)target, p[0], p[1], p[2], p[3], p[4], p[5]);
        HookLog("%s", buf);
        return false;
    }
    DWORD oldProt = 0;
    if (!VirtualProtect((void*)target, n, PAGE_EXECUTE_READWRITE, &oldProt)) {
        HookLog("hook: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }

    *back = (void*)(target + n);
    uint8_t patch[16];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)stub - (target + 5));
    memcpy(&patch[1], &rel, 4);
    memcpy((void*)target, patch, n);
    DWORD tmp = 0;
    VirtualProtect((void*)target, n, oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, n);
    (void)outVal;
    HookLog("hook: installed at %p (back=%p)", (void*)target, *back);
    return true;
}

bool Install() {
    if (g_installed) return true;
    g_installed = InstallAt(kScoreWriteRva, kOrig, sizeof(kOrig),
                            (void*)&csk_score_stub, &csk_hook_score, &csk_hook_back);
    return g_installed;
}

bool Installed() { return g_installed; }

uint32_t Score() {
    g_score = csk_hook_score;
    return g_score;
}

void Reset() {
    csk_hook_score = 0;
    g_score = 0;
}

bool InstallCombo() {
    if (g_combo_installed) return true;
    g_combo_installed = InstallAt(kComboWriteRva, kOrigCombo, sizeof(kOrigCombo),
                                  (void*)&csk_combo_stub, &csk_hook_combo, &csk_hook_combo_back);
    HookLog("combo hook: %s", g_combo_installed ? "ok" : "fail");
    return g_combo_installed;
}

bool ComboInstalled() { return g_combo_installed; }

uint32_t Combo() {
    g_combo = csk_hook_combo;
    return g_combo;
}

void ResetCombo() {
    csk_hook_combo = 0;
    g_combo = 0;
}

bool InstallSong() {
    if (g_song_installed) return true;
    csk_hook_song_target = (void*)kSongCallTarget;
    g_song_installed = InstallAt(kSongWriteRva, kOrigSong, sizeof(kOrigSong),
                                 (void*)&csk_song_stub, &csk_hook_song_id, &csk_hook_song_back);
    HookLog("song hook: %s (target=%p back=%p)", g_song_installed ? "ok" : "fail",
            csk_hook_song_target, csk_hook_song_back);
    return g_song_installed;
}

bool SongInstalled() { return g_song_installed; }

uint32_t SongId() {
    g_song_id = csk_hook_song_id;
    return g_song_id;
}

uint32_t SongPtr() { return csk_hook_song_ptr; }

uint32_t SongDiffEnc() {
    uint32_t ptr = csk_hook_song_ptr;
    if (!ptr) return 0;
    return *(volatile uint32_t*)(ptr + 4);
}

void ResetSong() {
    csk_hook_song_id = 0;
    csk_hook_song_ptr = 0;
    g_song_id = 0;
}

bool InstallDiff() {
    if (g_diff_installed) return true;
    g_diff_installed = InstallAt(kDiffWriteRva, kOrigDiff, sizeof(kOrigDiff),
                                 (void*)&csk_diff_stub, &csk_hook_diff, &csk_hook_diff_back);
    HookLog("diff hook: %s", g_diff_installed ? "ok" : "fail");
    return g_diff_installed;
}

bool DiffInstalled() { return g_diff_installed; }

uint32_t Diff() { return csk_hook_diff; }

void ResetDiff() { csk_hook_diff = 0; }

// —— FAST / LATE 计数 hook ——
// 原指令 5 字节：89 4A 58 (mov [edx+58],ecx) + 8B CE (mov ecx,esi)
static const uint8_t kOrigFl[5] = {0x89, 0x4A, 0x58, 0x8B, 0xCE};

bool InstallFastLate() {
    if (g_fl_installed) return true;
    g_fl_installed = InstallAt(kFlWriteRva, kOrigFl, sizeof(kOrigFl),
                               (void*)&csk_fl_stub, &csk_hook_fl_base, &csk_hook_fl_back);
    HookLog("fastlate hook: %s (back=%p)", g_fl_installed ? "ok" : "fail", csk_hook_fl_back);
    return g_fl_installed;
}

bool FastLateInstalled() { return g_fl_installed; }

uint32_t FlBase() { return csk_hook_fl_base; }

uint32_t FlMainBase() { return csk_hook_fl_main; }

void ResetFastLate() {
    csk_hook_fl_base = 0;
    csk_hook_fl_val = 0;
}


bool InstallMaxScore() {
    if (g_maxscore_installed) return true;
    g_maxscore_installed = InstallAt(kMaxScoreWriteRva, kOrigMaxScore, sizeof(kOrigMaxScore),
                                     (void*)&csk_maxscore_stub, &csk_hook_max_score,
                                     &csk_hook_max_score_back);
    HookLog("max_score hook: %s", g_maxscore_installed ? "ok" : "fail");
    return g_maxscore_installed;
}

bool MaxScoreInstalled() { return g_maxscore_installed; }

uint32_t MaxScore() {
    g_max_score = csk_hook_max_score;
    return g_max_score;
}

void ResetMaxScore() {
    csk_hook_max_score = 0;
    g_max_score = 0;
}


bool InstallBestScore() {
    if (g_bestscore_installed) return true;
    g_bestscore_installed = InstallAt(kBestScoreWriteRva, kOrigBestScore, sizeof(kOrigBestScore),
                                      (void*)&csk_bestscore_stub, &csk_hook_best_score,
                                      &csk_hook_best_score_back);
    HookLog("best_score hook: %s", g_bestscore_installed ? "ok" : "fail");
    return g_bestscore_installed;
}

bool BestScoreInstalled() { return g_bestscore_installed; }

uint32_t BestScore() { g_best_score = csk_hook_best_score; return g_best_score; }

void ResetBestScore() { csk_hook_best_score = 0; g_best_score = 0; }

} // namespace hook
} // namespace csk
