// chunithm-streamkit — sigscan.h
// 基于 IDA 风格特征码 (AOB) 的只读定位器。支持通配符 '?' 与 '??'。
// 解析规则：
//   none     : addr = hit + extra
//   abs32    : x86 绝对地址 (read u32 @ hit+extra)
//   rel32    : 相对调用/rip 相对 (hit + extra + 4 + rel32)
//   pointer  : 读 hit+extra 处保存的指针
//   lea32    : lea 指令形式，等同 rel32
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>
#include "memory.h"
#include "util.h"

namespace csk {

// 32 位进程的可扫描上限（x64 则用指针上限）
#ifdef _WIN64
static const uintptr_t kScanMaxAddr = (uintptr_t)0x7FFFFFFFFFFFull;
#else
static const uintptr_t kScanMaxAddr = (uintptr_t)0x7FFF0000u;
#endif

// 全地址空间只读区域枚举：对每个 MEM_COMMIT 且可读的区域调用 fn。
// fn 返回 false 停止枚举。
inline void ForEachReadableRegion(uintptr_t minAddr, uintptr_t maxAddr,
                                  const std::function<bool(uintptr_t, size_t)>& fn) {
    static const DWORD kReadable[] = {
        PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE,
        PAGE_WRITECOPY, PAGE_EXECUTE_WRITECOPY,
    };
    uintptr_t addr = minAddr;
    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < maxAddr) {
        if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) {
            addr += 0x1000;
            continue;
        }
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        size_t rsize = mbi.RegionSize;
        uintptr_t nxt = (rsize > 0) ? base + rsize : addr + 0x1000;
        if (mbi.State == MEM_COMMIT && rsize > 0 && !(mbi.Protect & PAGE_GUARD) &&
            base >= minAddr) {
            bool protOk = false;
            for (DWORD p : kReadable) {
                if ((mbi.Protect & 0xFF) == p) { protOk = true; break; }
            }
            if (protOk) {
                uintptr_t rb = std::max(base, minAddr);
                uintptr_t re = std::min(nxt, maxAddr);
                if (re > rb && !fn(rb, (size_t)(re - rb))) return;
            }
        }
        addr = (nxt > addr) ? nxt : addr + 0x1000;
    }
}

// 字符串签名定位（Chuni2Api 同款机制）：
//   在堆内存中搜索文本（可选要求 NUL 结尾），验证标记字节，
//   值地址 = 字符串起始 + value_offset。pick_highest=1 时取运行时
//   （地址最高）实例 —— 静态区通常低于 0x50000000。
struct StringSigSpec {
    std::string text;            // 搜索文本（UTF-8/ASCII）
    bool require_nul = true;     // 文本后必须跟 \0
    uintptr_t value_offset = 0;  // 值地址 = 命中点 + 此偏移
    uintptr_t marker_offset = 0; // 校验标记相对命中点的偏移（0=不校验）
    std::vector<uint8_t> marker; // 校验标记字节
    uintptr_t min_addr = 0x50000000;
    bool pick_highest = true;
    size_t max_hits = 3;
};

inline bool ScanStringHeap(const StringSigSpec& spec, uintptr_t& outValueAddr) {
    std::vector<uint8_t> needle(spec.text.begin(), spec.text.end());
    if (spec.require_nul) needle.push_back(0);
    if (needle.empty()) return false;

    std::vector<std::pair<uintptr_t, uintptr_t>> hits; // (值地址, 命中点)
    const size_t kChunk = 16 << 20;
    ForEachReadableRegion(spec.min_addr, kScanMaxAddr, [&](uintptr_t base, size_t size) {
        if (hits.size() >= spec.max_hits) return false;
        size_t done = 0;
        while (done < size) {
            size_t n = std::min(kChunk, size - done);
            std::vector<uint8_t> buf(n);
            if (!SafeRead(base + done, buf.data(), n)) break;
            size_t off = 0;
            while (off + needle.size() <= n) {
                size_t pos = off;
                bool found = false;
                for (; pos + needle.size() <= n; ++pos) {
                    if (memcmp(buf.data() + pos, needle.data(), needle.size()) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) break;
                uintptr_t hit = base + done + pos;
                bool markerOk = true;
                if (spec.marker_offset) {
                    uintptr_t ma = hit + spec.marker_offset;
                    markerOk = ma + spec.marker.size() <= base + size;
                    if (markerOk) {
                        std::vector<uint8_t> mb(spec.marker.size());
                        markerOk = SafeRead(ma, mb.data(), mb.size()) &&
                                   mb == spec.marker;
                    }
                }
                if (markerOk) {
                    hits.push_back({hit + spec.value_offset, hit});
                    if (hits.size() >= spec.max_hits) return false;
                }
                off = pos + 1;
            }
            if (n <= needle.size()) break;
            done += n - (needle.size() - 1); // 块间重叠，避免漏掉跨块字符串
        }
        return true;
    });

    if (hits.empty()) return false;
    uintptr_t best = 0;
    for (auto& h : hits) {
        if (spec.pick_highest) {
            if (h.first > best) best = h.first;
        } else if (!best) {
            best = h.first;
        }
    }
    if (!best) return false;
    outValueAddr = best;
    return true;
}

struct SigHit {
    uintptr_t addr = 0;   // 匹配点（虚拟地址）
};

class ModuleScanner {
public:
    bool Init(const std::string& moduleName) {
        HMODULE h = GetModuleHandleA(moduleName.c_str());
        if (!h) return false;
        base_ = (uintptr_t)h;
        size_ = ImageSize(h);
        name_ = moduleName;
        return size_ > 0;
    }

    uintptr_t base() const { return base_; }
    uintptr_t end() const { return base_ + size_; }
    const std::string& name() const { return name_; }

    // 解析特征码：字节间以空格分隔，'??' 或 '?' 为通配
    static bool ParsePattern(const std::string& text, std::vector<uint8_t>& bytes,
                             std::vector<bool>& mask) {
        bytes.clear(); mask.clear();
        std::string tok;
        for (char c : text) {
            if (c == ' ') {
                if (!tok.empty()) {
                    if (!ParseToken(tok, bytes, mask)) return false;
                    tok.clear();
                }
            } else {
                tok.push_back(c);
            }
        }
        if (!tok.empty() && !ParseToken(tok, bytes, mask)) return false;
        return !bytes.empty();
    }

    // 在模块镜像范围内扫描（仅可读已提交页），最多 maxHits 个结果
    std::vector<SigHit> Scan(const std::string& pattern, size_t maxHits = 1) {
        std::vector<SigHit> hits;
        std::vector<uint8_t> bytes;
        std::vector<bool> mask;
        if (!ParsePattern(pattern, bytes, mask)) return hits;

        std::vector<uint8_t> buf(0x10000);
        uintptr_t cur = base_;
        uintptr_t end = base_ + size_;
        while (cur < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery((LPCVOID)cur, &mbi, sizeof(mbi))) break;
            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY | PAGE_WRITECOPY))) {
                uintptr_t regionEnd = std::min<uintptr_t>(end, cur + mbi.RegionSize);
                for (uintptr_t p = cur; p + bytes.size() <= regionEnd;) {
                    size_t chunk = std::min(buf.size(), (size_t)(regionEnd - p));
                    if (!SafeRead(p, buf.data(), chunk)) break;
                    for (size_t i = 0; i + bytes.size() <= chunk; ++i) {
                        bool ok = true;
                        for (size_t j = 0; j < bytes.size(); ++j) {
                            if (mask[j] && buf[i + j] != bytes[j]) { ok = false; break; }
                        }
                        if (ok) {
                            hits.push_back({p + i});
                            if (hits.size() >= maxHits) return hits;
                        }
                    }
                    p += chunk;
                }
            }
            cur += mbi.RegionSize;
        }
        return hits;
    }

    // 根据命中点计算目标地址
    static uintptr_t Resolve(const SigHit& hit, const std::string& mode, uintptr_t extra, bool& ok) {
        ok = false;
        uintptr_t at = hit.addr + extra;
        if (mode == "none") { ok = true; return at; }
        if (mode == "abs32") {
            uint32_t v = 0;
            if (SafeReadT(at, v) && v) { ok = true; return v; }
            return 0;
        }
        if (mode == "rel32" || mode == "lea32") {
            int32_t rel = 0;
            if (SafeReadT(at, rel)) { ok = true; return at + 4 + (int64_t)rel; }
            return 0;
        }
        if (mode == "pointer") {
            uintptr_t v = 0;
            if (SafeReadT(at, v) && v) { ok = true; return v; }
            return 0;
        }
        return 0;
    }

    // 常用：模式 + 解析模式 + extra + 可选额外解引用次数
    bool ResolveBySig(const std::string& pattern, const std::string& mode,
                      uintptr_t extra, size_t matchIndex, uintptr_t derefCount,
                      uintptr_t& outAddr) {
        auto hits = Scan(pattern, matchIndex + 1);
        if (hits.size() <= matchIndex) return false;
        bool ok = false;
        uintptr_t addr = Resolve(hits[matchIndex], mode, extra, ok);
        if (!ok || !addr) return false;
        for (size_t i = 0; i < derefCount; ++i) {
            uintptr_t v = 0;
            if (!SafeReadT(addr, v) || !v) return false;
            addr = v;
        }
        outAddr = addr;
        return true;
    }

private:
    static bool ParseToken(const std::string& tok, std::vector<uint8_t>& bytes,
                           std::vector<bool>& mask) {
        if (tok == "?" || tok == "??") {
            bytes.push_back(0);
            mask.push_back(false);
            return true;
        }
        if (tok.size() != 2) return false;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nib(tok[0]), lo = nib(tok[1]);
        if (hi < 0 || lo < 0) return false;
        bytes.push_back((uint8_t)((hi << 4) | lo));
        mask.push_back(true);
        return true;
    }

    static size_t ImageSize(HMODULE h) {
        auto dos = (const IMAGE_DOS_HEADER*)h;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto nt = (const IMAGE_NT_HEADERS*)((uintptr_t)h + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return nt->OptionalHeader.SizeOfImage;
    }

    uintptr_t base_ = 0;
    size_t size_ = 0;
    std::string name_;
};

// —— 结构特征扫描 ——
// 在 center ± window 范围内扫描 [v, 0, 0, v, 0, 0, 0, 1.0f] 结构，
// 取其中最大的 v（score 这类偏移会漂、但结构固定的字段）。
inline bool ScanPair1F(uintptr_t center, uintptr_t window, uintptr_t& outAddr, uint32_t& outVal) {
    if (!center || window == 0) return false;
    uintptr_t base = center - window;
    size_t size = (size_t)window * 2;
    std::vector<uint8_t> buf(size);
    if (!SafeRead(base, buf.data(), size)) return false;
    uint32_t best = 0;
    uintptr_t bestAddr = 0;
    for (size_t i = 0; i + 32 <= size; i += 4) {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(buf.data() + i);
        if (p[1] != 0 || p[2] != 0 || p[4] != 0 || p[5] != 0 || p[6] != 0) continue;
        if (p[7] != 0x3F800000u) continue;      // 1.0f
        if (p[0] != p[3]) continue;             // 成对相同
        if (p[0] < 100 || p[0] > 1010000) continue;
        if (p[0] > best) { best = p[0]; bestAddr = base + i; }
    }
    if (!bestAddr) return false;
    outAddr = bestAddr;
    outVal = best;
    return true;
}

// —— 曲目结构扫描 ——
// 扫描 [难度 0..4][曲目ID 1..8000][0x40000|难度][9]，
// 这个组合在全内存唯一，命中即为「当前选中歌曲」。
// 返回结构基址（+0x00=难度，+0x04=曲目ID，+0x08=0x40000|难度）。
inline bool ScanSongStruct(uintptr_t& outAddr, uint32_t& outMusicId, uint32_t& outDiffEnc) {
    bool got = false;
    uintptr_t best = 0;
    uint32_t bestMid = 0, bestEnc = 0;
    const size_t kChunk = 16 << 20;
    ForEachReadableRegion(0x50000000, kScanMaxAddr, [&](uintptr_t base, size_t size) -> bool {
        if (size < 16) return true;
        size_t done = 0;
        while (done < size) {
            size_t n = std::min(kChunk, size - done);
            std::vector<uint8_t> buf(n);
            if (!SafeRead(base + done, buf.data(), n)) break;
            const uint32_t* a = reinterpret_cast<const uint32_t*>(buf.data());
            size_t cnt = n / 4;
            for (size_t i = 0; i + 4 <= cnt; ++i) {
                uint32_t diff = a[i];
                if (diff > 4) continue;
                uint32_t mid = a[i + 1];
                if (mid < 1 || mid > 8000) continue;
                if (a[i + 2] != (0x40000u + diff)) continue;
                if (a[i + 3] != 9) continue;
                uintptr_t addr = base + done + i * 4;
                if (!got || addr < best) {
                    got = true;
                    best = addr;
                    bestMid = mid;
                    bestEnc = a[i + 2];
                }
            }
            if (n <= 16) break;
            done += n - 12;   // 块间重叠 12 字节，避免漏掉跨块结构
        }
        return true;
    });
    if (!got) return false;
    outAddr = best;
    outMusicId = bestMid;
    outDiffEnc = bestEnc;
    return true;
}

// —— 当前难度扫描 ——
// 结构：[曲目ID][0x40000|难度][1][0×7][15][-1,-1,-1][0][0xFFFF]
// 该布局在内存里有多份（含滞后副本），取地址最低的一份实测跟随当前选择。
inline bool ScanCurrentDiff(uint32_t& outDiff, uint32_t& outMusicId) {
    bool got = false;
    uintptr_t best = 0;
    uint32_t bestDiff = 0, bestMid = 0;
    const size_t kChunk = 16 << 20;
    ForEachReadableRegion(0x50000000, kScanMaxAddr, [&](uintptr_t base, size_t size) -> bool {
        if (size < 64) return true;
        size_t done = 0;
        while (done < size) {
            size_t n = std::min(kChunk, size - done);
            std::vector<uint8_t> buf(n);
            if (!SafeRead(base + done, buf.data(), n)) break;
            const uint32_t* a = reinterpret_cast<const uint32_t*>(buf.data());
            size_t cnt = n / 4;
            for (size_t i = 0; i + 16 <= cnt; ++i) {
                uint32_t mid = a[i];
                if (mid < 1 || mid > 8000) continue;
                uint32_t enc = a[i + 1];
                if (enc < 0x40000 || enc > 0x40004) continue;
                if (a[i + 2] != 1) continue;
                bool z = true;
                for (int j = 3; j <= 9; ++j) if (a[i + j] != 0) { z = false; break; }
                if (!z) continue;
                if (a[i + 10] != 15) continue;
                if (a[i + 11] != 0xFFFFFFFF || a[i + 12] != 0xFFFFFFFF ||
                    a[i + 13] != 0xFFFFFFFF) continue;
                if (a[i + 14] != 0 || a[i + 15] != 0xFFFF) continue;
                uintptr_t addr = base + done + i * 4;
                if (!got || addr < best) {
                    got = true;
                    best = addr;
                    bestDiff = enc & 0xFFFF;
                    bestMid = mid;
                }
            }
            if (n <= 64) break;
            done += n - 60;   // 结构跨 64 字节，块间重叠
        }
        return true;
    });
    if (!got) return false;
    outDiff = bestDiff;
    outMusicId = bestMid;
    return true;
}

} // namespace csk
