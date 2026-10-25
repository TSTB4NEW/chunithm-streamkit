"""Parse CHUNITHM .srd UI layouts: texture name + CROP rect list.

Each texture section looks like:
    "TEX " ... <len><ascii name> ... "CROP" <4 bytes>
    then repeated entries:  0x65 0x45 <b> <x0,y0,x1,y1 : u16 LE>
Coordinates use a bottom-left origin (per community notes).

Usage:
  python srd_crops.py <file.srd> [name-substring] [--all]
"""
import re
import struct
import sys


def sections(d):
    out = []
    pos = 0
    while True:
        ti = d.find(b"TEX ", pos)
        if ti < 0:
            break
        ci = d.find(b"CROP", ti)
        if ci < 0:
            break
        seg = d[ti:ci]
        names = re.findall(rb"([ -~]{6,64})", seg)
        name = names[-1].decode("latin-1") if names else "?"
        rects = []
        p = ci + 8
        while p + 11 <= len(d) and d[p] == 0x65 and d[p + 1] == 0x45:
            x0, y0, x1, y1 = struct.unpack_from("<4H", d, p + 3)
            rects.append((x0, y0, x1, y1))
            p += 11
        out.append((name, rects, ci))
        pos = ci + 4
    return out


def main():
    path = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else None
    show_all = "--all" in sys.argv
    d = open(path, "rb").read()
    for name, rects, off in sections(d):
        if want and want not in name:
            continue
        print("%-28s %4d rects @0x%X" % (name, len(rects), off))
        if show_all:
            for r in rects:
                print("   %4d,%-4d -> %4d,%-4d   (%dx%d)" %
                      (r[0], r[1], r[2], r[3], r[2] - r[0], r[3] - r[1]))


if __name__ == "__main__":
    main()
