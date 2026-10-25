"""Slice the CHUNITHM result-screen judge panel out of CHU_UI_Result_01_v16.dds.

Emits the four purple rows (labels baked in) plus the LATE/FAST bars as small
RGBA PNGs with rounded corners, ready to use as CSS backgrounds.
"""
import base64
import io
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dds2png import read_dds

TEX = r"X:\Game\Chunithm\SDHD - MATE\data\surfboard\texture\CHU_UI_Result_01_v16.dds"
OUT_DIR = r"X:\杂物\文档\deepseek-harness\default-workspace\chunithm-streamkit\.tmp_ui\rows"
ROI = (600, 760, 1000, 1010)   # atlas region holding the judge panel


def bands(mask, axis):
    """Return (start, end) runs of True along axis."""
    proj = mask.any(axis=axis)
    runs, s = [], None
    for i, v in enumerate(proj):
        if v and s is None:
            s = i
        elif not v and s is not None:
            runs.append((s, i))
            s = None
    if s is not None:
        runs.append((s, len(proj)))
    return [r for r in runs if r[1] - r[0] > 6]


def main():
    img = read_dds(TEX).convert("RGBA")
    a = np.array(img)
    sub = a[ROI[1]:ROI[3], ROI[0]:ROI[2]]
    r, g, b = sub[..., 0].astype(int), sub[..., 1].astype(int), sub[..., 2].astype(int)

    purple = (b > 140) & (r < 190) & (g < 90)
    red = (r > 200) & (g < 120) & (b < 120)
    blue = (b > 200) & (r < 120) & (g > 110)

    os.makedirs(OUT_DIR, exist_ok=True)
    out = {}

    def emit(name, ys, xs, color_mask, radius=6):
        y0, y1 = ys
        x0, x1 = xs
        pad = 1
        box = (max(0, x0 - pad), max(0, y0 - pad), min(sub.shape[1], x1 + pad), min(sub.shape[0], y1 + pad))
        crop = Image.fromarray(sub[box[1]:box[3], box[0]:box[2]], "RGBA").convert("RGBA")
        w, h = crop.size
        mask = Image.new("L", (w, h), 0)
        ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, h - 1], radius=radius, fill=255)
        crop.putalpha(mask)
        buf = io.BytesIO()
        crop.save(buf, "PNG", optimize=True)
        out[name] = {"w": w, "h": h, "m": "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()}
        print("  %-6s %dx%d" % (name, w, h))
        return crop

    purple_rows = bands(purple, 1)
    print("purple rows:", purple_rows)
    # x extent of the purple rows (skip the small TAP/HOLD table which is above ROI)
    px = np.nonzero(purple.any(axis=0))[0]
    xs = (int(px.min()), int(px.max()) + 1)
    names = ["jc", "ju", "at", "mi"]
    tiles = []
    for i, (y0, y1) in enumerate(purple_rows[:4]):
        if i >= len(names):
            break
        tiles.append(emit(names[i], (y0, y1), xs, purple))

    # LATE/FAST 条在四行下面，先限定 y 范围，避免误抓上方的名次图标
    y_lo = purple_rows[-1][1] + 2 if purple_rows else 0
    red_b = [(a0, a1) for a0, a1 in bands(red, 1) if a0 >= y_lo]
    blue_b = [(a0, a1) for a0, a1 in bands(blue, 1) if a0 >= y_lo]
    print("late band:", red_b, "fast band:", blue_b)
    if red_b:
        y0, y1 = red_b[0]
        rx = np.nonzero(red[y0:y1].any(axis=0))[0]
        tiles.append(emit("late", (y0, y1), (int(rx.min()), int(rx.max()) + 1), red))
    if blue_b:
        y0, y1 = blue_b[0]
        bx = np.nonzero(blue[y0:y1].any(axis=0))[0]
        tiles.append(emit("fast", (y0, y1), (int(bx.min()), int(bx.max()) + 1), blue))

    json.dump(out, open(os.path.join(OUT_DIR, "rows.json"), "w"), separators=(",", ":"))
    # preview strip on dark background
    W = max(t.width for t in tiles) + 8
    H = sum(t.height + 4 for t in tiles) + 8
    sheet = Image.new("RGBA", (W, H), (16, 18, 28, 255))
    y = 4
    for t in tiles:
        sheet.alpha_composite(t, (4, y))
        y += t.height + 4
    sheet.save(os.path.join(OUT_DIR, "preview.png"))
    print("json:", os.path.getsize(os.path.join(OUT_DIR, "rows.json")), "bytes")


if __name__ == "__main__":
    main()
