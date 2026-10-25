"""Slice the big score digits from CHU_UI_Result_score_NUM_01_v250.dds using the
game's own CROP rects (parsed from CHU_UI_Result_01_v10.srd).

Emits web/score_digits.json: '0'-'9' and ',' as data URIs.
"""
import base64
import io
import json
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dds2png import read_dds
from srd_crops import sections

TEX = r"X:\Game\Chunithm\SDHD - MATE\data\surfboard\texture\CHU_UI_Result_score_NUM_01_v250.dds"
SRD = r"X:\Game\Chunithm\SDHD - MATE\data\surfboard\play\result\CHU_UI_Result_01_v10.srd"
OUT = r"X:\杂物\文档\deepseek-harness\default-workspace\chunithm-streamkit\web\score_digits.json"



def knock_out_white(img):
    """Flood-fill near-white background from the borders and clear its alpha."""
    from collections import deque
    img = img.convert("RGBA")
    w, h = img.size
    px = img.load()
    seen = [[False] * w for _ in range(h)]
    q = deque()
    def is_white(x, y):
        r, g, b, _ = px[x, y]
        return r > 242 and g > 242 and b > 242
    for x in range(w):
        for y in (0, h - 1):
            if is_white(x, y) and not seen[y][x]:
                seen[y][x] = True; q.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if is_white(x, y) and not seen[y][x]:
                seen[y][x] = True; q.append((x, y))
    while q:
        x, y = q.popleft()
        px[x, y] = (255, 255, 255, 0)
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] and is_white(nx, ny):
                seen[ny][nx] = True
                q.append((nx, ny))
    return img


def main():
    d = open(SRD, "rb").read()
    rects = None
    for name, rs, off in sections(d):
        if "score_NUM_01_v250" in name:
            rects = rs
            break
    if not rects or len(rects) < 55:
        raise SystemExit("score digit rects not found")
    img = read_dds(TEX).convert("RGBA")

    chars = "0123456789,"
    out = {}
    for i, ch in enumerate(chars):
        r = rects[i]           # grey row = first 11
        # 严格按图集步距取整格（82×106），不额外裁剪：
        # 这样按高度缩放后 advance 与游戏完全一致，不会互相压盖
        if ch == ",":
            cell_w, cell_h = 46, 70
            x0, y0 = max(0, r[0] - 2), r[1]
        else:
            cell_w, cell_h = 82, 106
            x0, y0 = max(0, r[0] - 2), r[1]
        crop = img.crop((x0, y0, x0 + cell_w, y0 + cell_h))
        crop = knock_out_white(crop)
        canvas = Image.new("RGBA", (cell_w, cell_h), (0, 0, 0, 0))
        canvas.alpha_composite(crop, (0, 0))
        buf = io.BytesIO()
        canvas.save(buf, "PNG", optimize=True)
        out[ch] = {"w": cell_w, "h": cell_h,
                   "m": "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()}
        print("  %r %dx%d" % (ch, cell_w, cell_h))
    json.dump({"chars": out}, open(OUT, "w"), separators=(",", ":"))
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
