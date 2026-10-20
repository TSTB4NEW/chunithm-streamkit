#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
chunithm-streamkit 内存探针（只读，外部进程）v2
================================================
在游戏运行时，通过「内存变化检测」定位判定计数 / 连击 / 分数的实时地址。

用法（两种模式）：

  1) 找实时判定/分数地址（推荐）：
       python mem_probe.py diff 3
     - 先进入一首歌，开始打（保持有 NOTE 判定落下）；
     - 在另一个窗口运行上面的命令，它会对游戏内存做两次快照（间隔 3 秒），
       输出这段时间里「数值增长」的地址 —— 判定计数会以 +1/+2 递增，
       分数会 +1010 左右，连击会 +1。
     - 把输出（或 mem_probe_dump.txt）发给我，我据此写出配置。

  2) 找序列化字段名的运行时地址（辅助诊断）：
       python mem_probe.py strings

只读（PROCESS_VM_READ + PROCESS_QUERY_INFORMATION），不写游戏内存。
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import os
import time
import numpy as np

MEM_COMMIT = 0x1000
READABLE = {0x02, 0x04, 0x20, 0x40, 0x08, 0x80}
WRITABLE = {0x04, 0x40, 0x80}  # RW / EXEC_RW / EXEC_WC

k32 = ctypes.windll.kernel32


class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", wt.LPVOID),
        ("AllocationBase", wt.LPVOID),
        ("AllocationProtect", wt.DWORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
    ]


def find_pid(name="chusanApp.exe"):
    snap = k32.CreateToolhelp32Snapshot(0x00000002, 0)
    if snap == -1:
        return None

    class PE(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                    ("th32ProcessID", wt.DWORD), ("th32DefaultHeapID", wt.ULONG),
                    ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                    ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", wt.LONG),
                    ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_char * 260)]
    pe = PE()
    pe.dwSize = ctypes.sizeof(PE)
    pid = None
    if k32.Process32First(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.decode('mbcs', 'ignore').lower() == name.lower():
                pid = pe.th32ProcessID
                break
            if not k32.Process32Next(snap, ctypes.byref(pe)):
                break
    k32.CloseHandle(snap)
    return pid


def read(handle, addr, size):
    buf = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(handle, wt.LPVOID(addr), buf, size, ctypes.byref(n))
    return buf.raw[:n.value] if ok else b''


def enum_writable_regions(handle):
    """枚举所有可写、已提交区域（堆 + 镜像 .data），用于数值快照"""
    addr = 0
    out = []
    while addr < 0x7FFF0000:
        mbi = MBI()
        if k32.VirtualQueryEx(handle, wt.LPVOID(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0:
            addr += 0x1000
            continue
        base = ctypes.cast(mbi.BaseAddress, ctypes.c_void_p).value or 0
        size = mbi.RegionSize
        prot = mbi.Protect & 0xFF
        nxt = base + size if size else addr + 0x1000
        if mbi.State == MEM_COMMIT and size and prot in WRITABLE and not (mbi.Protect & 0x100):
            out.append((base, size))
        addr = nxt if nxt > addr else addr + 0x1000
    return out


def collect(handle, regions, lo, hi):
    """向量化收集 [lo,hi] 内的 u32 地址与值（numpy 数组，不建 Python dict）"""
    addrs = []
    vals = []
    for base, size in regions:
        data = read(handle, base, size)
        if not data:
            continue
        arr = np.frombuffer(data, dtype='<u4', count=len(data) // 4)
        idx = np.nonzero((arr >= lo) & (arr <= hi))[0]
        if idx.size:
            addrs.append((base + (idx.astype(np.uint32) << 2)).astype(np.uint32))
            vals.append(arr[idx].astype(np.uint32))
    if addrs:
        return np.concatenate(addrs), np.concatenate(vals)
    return np.array([], dtype=np.uint32), np.array([], dtype=np.uint32)


def diff_mode(handle, seconds):
    print("正在枚举可写内存区域……", flush=True)
    regions = enum_writable_regions(handle)
    total = sum(s for _, s in regions)
    print(f"可写区域 {len(regions)} 个，共 {total/1024/1024:.0f} MB", flush=True)

    LO, HI = 1, 2_000_000
    print("快照 A（此时应正在打歌）……", flush=True)
    addrs_a, vals_a = collect(handle, regions, LO, HI)
    print(f"  候选 u32 地址 {len(addrs_a)} 个", flush=True)
    order = np.argsort(addrs_a, kind='stable')
    addrs_a = addrs_a[order]
    vals_a = vals_a[order]
    del order

    print(f"等待 {seconds} 秒……", flush=True)
    time.sleep(seconds)

    print("快照 B 并做差分……", flush=True)
    changed_addr = []
    changed_old = []
    changed_new = []
    for k, (base, size) in enumerate(regions):
        data = read(handle, base, size)
        if not data:
            continue
        arr = np.frombuffer(data, dtype='<u4', count=len(data) // 4)
        idx = np.nonzero((arr >= LO) & (arr <= HI))[0]
        if not idx.size:
            continue
        addrs_b = (base + (idx.astype(np.uint32) << 2)).astype(np.uint32)
        vals_b = arr[idx].astype(np.uint32)
        pos = np.searchsorted(addrs_a, addrs_b)
        valid = pos < len(addrs_a)
        if not valid.any():
            continue
        pos_c = np.clip(pos, 0, len(addrs_a) - 1)
        match = valid & (addrs_a[pos_c] == addrs_b)
        if not match.any():
            continue
        old = vals_a[pos_c[match]].astype(np.int64)
        new = vals_b[match].astype(np.int64)
        delta = new - old
        good = (delta >= 1) & (delta <= 50000)
        if good.any():
            m = match
            gi = np.nonzero(good)[0]
            changed_addr.append(addrs_b[m][gi])
            changed_old.append(old[gi])
            changed_new.append(new[gi])
        if k % 400 == 0:
            print(f"  B {k}/{len(regions)}", flush=True)

    print("汇总结果……", flush=True)

    if changed_addr:
        ca = np.concatenate(changed_addr)
        co = np.concatenate(changed_old)
        cn = np.concatenate(changed_new)
        cd = cn - co
    else:
        ca = co = cn = cd = np.array([], dtype=np.int64)

    print(f"\n=== {len(ca)} 个地址在此 {seconds} 秒内数值增长（按增量降序）===")
    lines = []
    if len(ca):
        order = np.argsort(-cd)  # 降序：大增量（分数/连击）排前面
        for a, o, n, d in zip(ca[order], co[order], cn[order], cd[order]):
            line = f"  0x{a:08X}  {o:>10} -> {n:>10}   +{d}"
            print(line)
            lines.append(line)
            if len(lines) >= 3000:
                break

    # 聚簇：地址连续（间隔<=16 字节）且都在变化，通常是同一结构体的相邻字段
    if len(ca):
        sa = np.sort(ca)
        gaps = np.diff(sa)
        clusters = []
        start = 0
        for i, g in enumerate(gaps):
            if g > 16:
                if i - start + 1 >= 3:
                    clusters.append(sa[start:i + 1])
                start = i + 1
        if len(sa) - start >= 3:
            clusters.append(sa[start:])
        if clusters:
            print("\n=== 疑似连续字段簇（4 个相邻计数 = 判定计数）===")
            for c in clusters[:20]:
                print("  " + "  ".join(f"0x{a:08X}" for a in c))

    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mem_probe_dump.txt'),
              'w', encoding='utf-8') as f:
        f.write(f"mem_probe diff ({seconds}s), changed={len(ca)}\n")
        f.write('\n'.join(lines) + '\n')
    print("\n结果已写入 mem_probe_dump.txt")


def strings_mode(handle):
    CANDIDATES = [b'missNum\0', b'justiceNum\0', b'attackNum\0', b'comboNum\0',
                  b'jhComboNum\0', b'maxCombo\0', b'musicId\0', b'musicDiff\0',
                  b'judgeCritical\0', b'judgeJustice\0', b'judgeAttack\0']
    print("扫描可读内存中的字段名……")
    results = []
    addr = 0
    while addr < 0x7FFF0000:
        mbi = MBI()
        if k32.VirtualQueryEx(handle, wt.LPVOID(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0:
            addr += 0x1000
            continue
        base = ctypes.cast(mbi.BaseAddress, ctypes.c_void_p).value or 0
        size = mbi.RegionSize
        prot = mbi.Protect & 0xFF
        nxt = base + size if size else addr + 0x1000
        if mbi.State == MEM_COMMIT and size and prot in READABLE:
            data = read(handle, base, min(size, 32 << 20))
            for cand in CANDIDATES:
                off = data.find(cand)
                while off != -1:
                    results.append((base + off, cand.rstrip(b'\0').decode()))
                    off = data.find(cand, off + 1)
        addr = nxt if nxt > addr else addr + 0x1000

    if not results:
        print("未找到字段名（它们可能只在镜像 .rdata，未拷贝到可写/可读堆）。")
        return
    best = {}
    for va, label in results:
        if label not in best or va > best[label][0]:
            best[label] = (va, label)
    for label in sorted(best):
        va = best[label][0]
        print(f"  {label:16s} @ 0x{va:08X}")


def main():
    # 解析参数：--pid <PID> [diff|strings] [seconds]
    pid = None
    mode = 'diff'
    seconds = 3.0
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--pid' and i + 1 < len(args):
            pid = int(args[i + 1])
            i += 2
            continue
        if a in ('diff', 'strings'):
            mode = a
            i += 1
            continue
        # 纯数字视为秒数
        try:
            seconds = float(a)
        except ValueError:
            pass
        i += 1

    if not pid:
        pid = find_pid()
    if not pid:
        print("未找到 chusanApp.exe。可用 --pid <PID> 显式指定（任务管理器→详细信息→PID 列）。")
        sys.exit(1)
    print(f"chusanApp.exe PID = {pid}")
    handle = k32.OpenProcess(0x0010 | 0x0400, False, pid)
    if not handle:
        print("OpenProcess 失败（可能需要管理员权限，或 PID 错误）。")
        sys.exit(1)

    if mode == 'strings':
        strings_mode(handle)
    else:
        diff_mode(handle, seconds)
    k32.CloseHandle(handle)


if __name__ == '__main__':
    main()
