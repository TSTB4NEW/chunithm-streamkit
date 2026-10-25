"""极简 minidump 读取：列出模块、线程，并在栈内存里扫描返回地址。"""
import struct
import sys


class Dump:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        sig, ver, n, rva = struct.unpack_from('<IIII', self.d, 0)[0:4]
        self.streams = {}
        for i in range(n):
            t, size, r = struct.unpack_from('<III', self.d, rva + i * 12)
            self.streams.setdefault(t, (size, r))

    def modules(self):
        size, off = self.streams[4]
        n, = struct.unpack_from('<I', self.d, off)
        out = []
        for i in range(n):
            b = off + 4 + i * 108
            base, isize = struct.unpack_from('<QI', self.d, b)
            name_rva, = struct.unpack_from('<I', self.d, b + 20)
            ln, = struct.unpack_from('<I', self.d, name_rva)
            name = self.d[name_rva + 4:name_rva + 4 + ln].decode('utf-16-le', 'replace')
            out.append((base, isize, name))
        return out

    def threads(self):
        size, off = self.streams[3]
        n, = struct.unpack_from('<I', self.d, off)
        out = []
        for i in range(n):
            b = off + 4 + i * 48
            tid, susp, pcls, prio = struct.unpack_from('<IIII', self.d, b)
            start, dsize, drva = struct.unpack_from('<QII', self.d, b + 24)
            out.append((tid, start, dsize, drva))
        return out


def short(name):
    return name.replace('\\', '/').split('/')[-1]


def main(path, only=None):
    dmp = Dump(path)
    mods = sorted(dmp.modules())

    def resolve(x):
        for base, size, name in mods:
            if base <= x < base + size:
                return '%s+0x%X' % (short(name), x - base)
        return None

    for tid, start, dsize, drva in dmp.threads():
        mem = dmp.d[drva:drva + dsize]
        hits = []
        for i in range(0, len(mem) - 3, 4):
            v, = struct.unpack_from('<I', mem, i)
            r = resolve(v)
            if r:
                hits.append((start + i, r))
        if only and not any(only in h[1] for h in hits):
            continue
        print('=== thread %d  stack=%#x size=%d hits=%d' % (tid, start, dsize, len(hits)))
        for addr, r in hits[:60]:
            print('    %#010x  %s' % (addr, r))
        print()


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'ucrtbase')
