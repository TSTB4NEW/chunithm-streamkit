"""minidump 栈回溯（32 位进程）：解析模块、线程上下文，按 EBP 链回溯。"""
import struct
import sys

BS = chr(92)  # backslash


class Dmp:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        _, _, n, rva = struct.unpack_from('<IIII', self.d, 0)
        self.streams = {}
        for i in range(n):
            t, size, r = struct.unpack_from('<III', self.d, rva + i * 12)
            self.streams.setdefault(t, (size, r))
        self.mods = self._modules()
        self.threads = self._threads()

    def _modules(self):
        size, off = self.streams[4]
        n, = struct.unpack_from('<I', self.d, off)
        out = []
        for i in range(n):
            b = off + 4 + i * 108
            base, isize = struct.unpack_from('<QI', self.d, b)
            nrva, = struct.unpack_from('<I', self.d, b + 20)
            ln, = struct.unpack_from('<I', self.d, nrva)
            name = self.d[nrva + 4:nrva + 4 + ln].decode('utf-16-le', 'replace')
            out.append((base, isize, name.split(BS)[-1]))
        out.sort()
        return out

    def _threads(self):
        size, off = self.streams[3]
        n, = struct.unpack_from('<I', self.d, off)
        out = []
        for i in range(n):
            b = off + 4 + i * 48
            tid, = struct.unpack_from('<I', self.d, b)
            start, dsize, drva = struct.unpack_from('<QII', self.d, b + 24)
            csize, crva = struct.unpack_from('<II', self.d, b + 40)
            ctx = self.d[crva:crva + csize]
            ebp = struct.unpack_from('<I', ctx, 0xB8)[0]
            eip = struct.unpack_from('<I', ctx, 0xBC)[0]
            esp = struct.unpack_from('<I', ctx, 0xC8)[0]
            out.append(dict(tid=tid, stack=(start, dsize, drva),
                            ebp=ebp, eip=eip, esp=esp))
        return out

    def resolve(self, x):
        for base, size, name in self.mods:
            if base <= x < base + size:
                return '%s+0x%X' % (name, x - base)
        return None

    def read_stack(self, tid):
        for t in self.threads:
            if t['tid'] == tid:
                start, dsize, drva = t['stack']
                return start, self.d[drva:drva + dsize]
        return None, None


def walk(dmp, tid, limit=40):
    start, mem = dmp.read_stack(tid)
    if mem is None:
        print('thread not found')
        return
    def rd32(addr):
        off = addr - start
        if 0 <= off <= len(mem) - 4:
            return struct.unpack_from('<I', mem, off)[0]
        return None
    t = next(x for x in dmp.threads if x['tid'] == tid)
    print('thread %d  eip=%s' % (tid, dmp.resolve(t['eip']) or hex(t['eip'])))
    ebp = t['ebp']
    seen = set()
    for i in range(limit):
        if ebp in seen or not ebp:
            break
        seen.add(ebp)
        ret = rd32(ebp + 4)
        nxt = rd32(ebp)
        if ret is None:
            break
        sym = dmp.resolve(ret)
        if sym:
            print('  #%02d %#010x %s' % (i, ret, sym))
        ebp = nxt


if __name__ == '__main__':
    dmp = Dmp(sys.argv[1])
    if len(sys.argv) > 2 and sys.argv[2] != '-':
        walk(dmp, int(sys.argv[2]))
    else:
        for t in dmp.threads:
            print('tid %-6d eip %-40s ebp=%#x esp=%#x' %
                  (t['tid'], dmp.resolve(t['eip']) or hex(t['eip']), t['ebp'], t['esp']))
