// chunithm-streamkit — memory.h
// 进程内只读内存访问。全部经 ReadProcessMemory(自身进程) 实现：
//  1) 越界/未映射地址只会失败返回，不会触发 SEH/崩溃；
//  2) 绝不写目标内存 —— 这是“只读采集”的核心保证之一。
#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>

namespace csk {

inline bool SafeRead(uintptr_t addr, void* buf, size_t n) {
    if (!addr || !buf || !n) return false;
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, buf, n, &done) && done == n;
}

template <typename T>
inline bool SafeReadT(uintptr_t addr, T& out) {
    return SafeRead(addr, &out, sizeof(T));
}

inline uintptr_t ReadPtr(uintptr_t addr) {
    uintptr_t v = 0;
    SafeReadT(addr, v);
    return v;
}

// 指针链：addr = base + chain[0]；此后逐项 addr = *(addr) + chain[i]。
// 返回最终取值地址（未读值本身）。
// chain[0] 为 0xFFFFFFFF 时表示“上一步地址不加偏移直接解引用”。
inline bool ResolveChain(uintptr_t base, const std::vector<uintptr_t>& chain, uintptr_t& outAddr) {
    if (chain.empty()) return false;
    uintptr_t addr = base + chain[0];
    for (size_t i = 1; i < chain.size(); ++i) {
        uintptr_t p = 0;
        if (!SafeReadT(addr, p) || !p) return false;
        addr = p + chain[i];
    }
    outAddr = addr;
    return addr != 0;
}

} // namespace csk
