#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""u16 版内存探针：判定计数通常是 u16（0..3000），2 字节对齐。"""
import ctypes, ctypes.wintypes as wt, time, sys, os
import numpy as np

k32 = ctypes.windll.kernel32

class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", wt.LPVOID), ("AllocationBase", wt.LPVOID),
                ("AllocationProtect", wt.DWORD), ("RegionSize", ctypes.c_size_t),
                ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]

pid = int(sys.argv[1])
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
h = k32.OpenProcess(0x0010 | 0x0400, False, pid)
print('OpenProcess', h, 'err', k32.GetLastError(), flush=True)
if not h:
    sys.exit(1)

def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(h, wt.LPVOID(addr), buf, size, ctypes.byref(n))
    return buf.raw[:n.value] if ok else b''

regions = []
addr = 0
while addr < 0x7FFF0000:
    mbi = MBI()
    if k32.VirtualQueryEx(h, wt.LPVOID(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0:
        addr += 0x1000; continue
    base = ctypes.cast(mbi.BaseAddress, ctypes.c_void_p).value or 0
    size = mbi.RegionSize
    prot = mbi.Protect & 0xFF
    nxt = base + size if size else addr + 0x1000
    if mbi.State == 0x1000 and size and prot in (0x04, 0x40, 0x80) and not (mbi.Protect & 0x100):
        regions.append((base, size))
    addr = nxt if nxt > addr else addr + 0x1000
print(f'regions {len(regions)}, {sum(s for _,s in regions)/1024/1024:.0f} MB', flush=True)

def collect16(lo, hi):
    addrs, vals = [], []
    for base, size in regions:
        data = read(base, size)
        if not data:
            continue
        arr = np.frombuffer(data, dtype='<u2', count=len(data)//2)
        idx = np.nonzero((arr >= lo) & (arr <= hi))[0]
        if idx.size:
            addrs.append((base + (idx.astype(np.uint32) << 1)).astype(np.uint32))
            vals.append(arr[idx].astype(np.uint32))
    if addrs:
        return np.concatenate(addrs), np.concatenate(vals)
    return np.array([], dtype=np.uint32), np.array([], dtype=np.uint32)

LO, HI = 1, 5000
print('u16 snapshot A ……', flush=True)
addrs_a, vals_a = collect16(LO, HI)
print(f'  candidates {len(addrs_a)}', flush=True)
order = np.argsort(addrs_a, kind='stable')
addrs_a = addrs_a[order]; vals_a = vals_a[order]
print(f'wait {seconds}s ……', flush=True)
time.sleep(seconds)
print('u16 snapshot B + diff ……', flush=True)
ca, co, cn = [], [], []
for base, size in regions:
    data = read(base, size)
    if not data: continue
    arr = np.frombuffer(data, dtype='<u2', count=len(data)//2)
    idx = np.nonzero((arr >= LO) & (arr <= HI))[0]
    if not idx.size: continue
    addrs_b = (base + (idx.astype(np.uint32) << 1)).astype(np.uint32)
    vals_b = arr[idx].astype(np.uint32)
    pos = np.searchsorted(addrs_a, addrs_b)
    valid = pos < len(addrs_a)
    if not valid.any(): continue
    pos_c = np.clip(pos, 0, len(addrs_a)-1)
    match = valid & (addrs_a[pos_c] == addrs_b)
    if not match.any(): continue
    old = vals_a[pos_c[match]].astype(np.int64)
    new = vals_b[match].astype(np.int64)
    d = new - old
    good = (d >= 1) & (d <= 300)
    if good.any():
        gi = np.nonzero(good)[0]
        ca.append(addrs_b[match][gi]); co.append(old[gi]); cn.append(new[gi])
ca = np.concatenate(ca); co = np.concatenate(co); cn = np.concatenate(cn); cd = cn - co
print(f'=== {len(ca)} 个 u16 地址增长（降序）===', flush=True)
lines = []
order = np.argsort(-cd)
for a, o, n, d in zip(ca[order], co[order], cn[order], cd[order]):
    line = f"  0x{a:08X}  {o:>6} -> {n:>6}  +{d}"
    print(line, flush=True)
    lines.append(line)
    if len(lines) >= 5000: break

# 聚簇（连续 2 字节地址）
sa = np.sort(ca)
gaps = np.diff(sa)
clusters = []; start = 0
for i, g in enumerate(gaps):
    if g > 4:
        if i - start + 1 >= 3:
            clusters.append(sa[start:i+1])
        start = i + 1
if len(sa) - start >= 3:
    clusters.append(sa[start:])
if clusters:
    print('=== 连续簇（4 个相邻 = 判定计数候选）===', flush=True)
    for c in clusters[:30]:
        print('  ' + '  '.join(f'0x{a:08X}' for a in c), flush=True)
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mem_probe16_dump.txt'), 'w') as f:
    f.write('\n'.join(lines) + '\n')
k32.CloseHandle(h)
print('done', flush=True)
