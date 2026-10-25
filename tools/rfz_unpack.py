"""RFZ -> YABX -> glyphs + DDS atlases, per the published unpack spec.

Writes: decompressed.bin, metadata.json, glyphs.csv, pageN.png
Usage: python rfz_unpack.py <input.rfz> [out_dir]
"""
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

# ---------------- LZW (YbLzwDecoder: MSB-first, early change, reset at 4094) ----


class Bits:
    def __init__(self, data, pos):
        self.d = data
        self.p = pos
        self.buf = 0
        self.n = 0

    def read(self, nbits):
        need = (nbits - self.n + 7) // 8
        for _ in range(need):
            if self.p >= len(self.d):
                return None
            self.buf = ((self.buf << 8) | self.d[self.p]) & 0xFFFFFFFF
            self.p += 1
        shift = 8 * need - nbits + self.n
        code = self.buf >> shift
        self.buf &= (1 << shift) - 1
        self.n += 8 * need - nbits
        return code


MAX_ENTRIES = 4096
RESET_AT = MAX_ENTRIES - 2   # 4094
INIT_BITS = 9
INIT_TOKENS = 256


def lzw_decompress(data):
    last = [0] * MAX_ENTRIES
    nxt = [-1] * MAX_ENTRIES
    for i in range(MAX_ENTRIES):
        last[i] = i if i < 256 else 0
        nxt[i] = -1

    out = bytearray()
    win = bytearray(4096)

    def chain(code):
        """Return (first_byte, full_bytes) for a code."""
        n = 4096
        c = code
        while nxt[c] != -1:
            n -= 1
            win[n] = last[c]
            c = nxt[c]
        root = last[c]
        return root, bytes([root]) + bytes(win[n:])

    bits = Bits(data, 4)
    code_bits = INIT_BITS
    next_code = INIT_TOKENS

    code = bits.read(code_bits)
    if code is None:
        return bytes(out)
    first_byte, seq = chain(code)
    out += seq
    prev = code

    while True:
        code = bits.read(code_bits)
        if code is None:
            break
        if code > next_code:
            break
        if code < next_code:
            fb, seq = chain(code)
            out += seq
        else:
            _, seq = chain(prev)
            out += seq
            out.append(first_byte)
            fb = first_byte

        if next_code != RESET_AT:
            last[next_code] = fb
            nxt[next_code] = prev
            old = next_code
            next_code += 1
            if old + 2 == (1 << code_bits):
                code_bits += 1
        else:
            for i in range(INIT_TOKENS, MAX_ENTRIES):
                nxt[i] = -1
            next_code = INIT_TOKENS
            code_bits = INIT_BITS
            # inline next code, no table entry
            code = bits.read(code_bits)
            if code is None:
                break
            if code < next_code:
                fb, seq = chain(code)
                out += seq
            else:
                _, seq = chain(prev)
                out += seq
                out.append(first_byte)
                fb = first_byte
            prev = code
            first_byte = fb
            continue
        first_byte = fb
        prev = code
    return bytes(out)


# ---------------- YABX parsing ----------------


def read_u32(d, p):
    return struct.unpack_from("<I", d, p)[0], p + 4


def read_u16(d, p):
    return struct.unpack_from("<H", d, p)[0], p + 2


def read_tlv_string(d, p):
    size, p = read_u32(d, p)
    slen, p = read_u16(d, p) if size >= 2 else (0, p)
    s = d[p:p + max(0, slen - 1)]
    return s.split(b"\0")[0].decode("latin-1"), p + max(0, size - 2)


def parse_yabx(d):
    assert d[:4] == b"YABX", d[:4]
    p = d.find(b"RHFONTDB")
    if p < 0:
        raise SystemExit("RHFONTDB anchor not found")
    # id string starts 4 bytes before the chars: size(4) + len(2)
    p -= 6
    fields = {}
    for name in ("id", "platform", "library", "name", "comment"):
        s, p = read_tlv_string(d, p)
        fields[name] = s
    fields["flags"], p = read_u32(d, p)
    for name in ("point", "max_ascent", "max_descent", "max_glyph_w", "max_glyph_h"):
        fields[name], p = read_u16(d, p)
    fields["tex_page"], p = read_u32(d, p)
    for name in ("tex_w", "tex_h", "tex_last_h", "glyph_margin", "glyph_cnt"):
        fields[name], p = read_u16(d, p)

    # glyph array: <u32 fieldsize><u32 count><count*u16>
    _fieldsize, p = read_u32(d, p)
    count, p = read_u32(d, p)
    ids = list(struct.unpack_from("<%dH" % count, d, p))
    p += 2 * count

    # glyph object definitions: 10-byte object table header, then 6+body_size each
    q = p + 10
    if q + 6 > len(d):
        raise SystemExit("glyph table missing")
    marker, body_size = struct.unpack_from("<HI", d, q)
    if marker != 3 or body_size != 30:
        # tolerate a slightly different table header size
        found = -1
        for probe in range(p, min(p + 64, len(d) - 6)):
            mk, bs = struct.unpack_from("<HI", d, probe)
            if mk == 3 and bs == 30:
                found = probe
                break
        if found < 0:
            raise SystemExit("glyph table header not found near %d" % p)
        q = found
    glyphs = []
    for i in range(count):
        marker, body_size = struct.unpack_from("<HI", d, q)
        if marker != 3 or body_size != 30:
            raise SystemExit("glyph table broken at %d (marker=%d size=%d)" % (i, marker, body_size))
        vals = struct.unpack_from("<11H", d, q + 6)
        glyphs.append({
            "code": vals[0], "cell_inc_x": vals[1], "cell_inc_y": vals[2],
            "page": vals[3], "origin_x": vals[4], "origin_y": vals[5],
            "x1": vals[6], "y1": vals[7], "x2": vals[8], "y2": vals[9],
            "kern": vals[10],
        })
        q += 6 + body_size
    return fields, glyphs, q


def extract_dds(d, start):
    pages = []
    p = start
    while True:
        p = d.find(b"DDS ", p)
        if p < 0:
            break
        size = struct.unpack_from("<I", d, p + 4)[0]
        if size != 124:
            p += 4
            continue
        h = d[p:p + 128]
        height = struct.unpack_from("<I", h, 12)[0]
        width = struct.unpack_from("<I", h, 16)[0]
        pitch = struct.unpack_from("<I", h, 20)[0]
        fmt = struct.unpack_from("<I", h, 76 + 8)[0]  # ddspf.dwFourCC at +84
        bitcount = struct.unpack_from("<H", h, 88)[0]
        if fmt != 0 or bitcount != 16:
            p += 4
            continue
        nbytes = 128 + pitch
        raw = d[p + 128:p + 128 + pitch]
        if len(raw) < pitch:
            break
        pages.append((width, height, raw))
        p += nbytes
    return pages


def save_pages(pages, out_dir):
    paths = []
    for i, (w, h, raw) in enumerate(pages):
        a = np.frombuffer(raw, dtype="<u2").reshape(h, w)
        r = ((a >> 8) & 0xF) * 17
        g = ((a >> 4) & 0xF) * 17
        b = (a & 0xF) * 17
        al = ((a >> 12) & 0xF) * 17
        img = np.dstack([r, g, b, al]).astype(np.uint8)
        path = os.path.join(out_dir, "page%d.png" % i)
        Image.fromarray(img, "RGBA").save(path)
        paths.append(path)
        print("page%d %dx%d -> %s" % (i, w, h, path))
    return paths


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "rfz_out"
    os.makedirs(out, exist_ok=True)
    d = open(src, "rb").read()
    if d[:2] != b"YS" or d[2] != 2:
        raise SystemExit("not an RFZ v2 file")
    dec = lzw_decompress(d)
    open(os.path.join(out, "decompressed.bin"), "wb").write(dec)
    print("decompressed:", len(dec), "starts:", dec[:16])
    fields, glyphs, tex_start = parse_yabx(dec)
    print("metadata:", {k: v for k, v in fields.items() if not isinstance(v, str)})
    json.dump(fields, open(os.path.join(out, "metadata.json"), "w"), indent=1)
    with open(os.path.join(out, "glyphs.csv"), "w", encoding="utf-8") as f:
        f.write("code,cell_inc_x,page,origin_x,origin_y,x1,y1,x2,y2,kern\n")
        for g in glyphs:
            f.write("%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n" % (
                g["code"], g["cell_inc_x"], g["page"], g["origin_x"], g["origin_y"],
                g["x1"], g["y1"], g["x2"], g["y2"], g["kern"]))
    pages = extract_dds(dec, tex_start)
    print("DDS pages:", [(w, h) for w, h, _ in pages])
    save_pages(pages, out)


if __name__ == "__main__":
    main()
