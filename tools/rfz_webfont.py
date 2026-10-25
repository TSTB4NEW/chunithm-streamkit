"""Emit web/glyphs.json: per-character PNG masks as data URIs + metrics.

The plugin's static server only serves files at the web root (no subfolders),
so a single JSON with inline data URIs avoids adding files and extra requests.

Usage: python rfz_webfont.py <rfz_out_dir> <out_json>
"""
import base64
import io
import json
import os
import sys

from PIL import Image

CHARS = ("0123456789" "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
         " /:,.+-%")


def main():
    src, out_path = sys.argv[1], sys.argv[2]
    meta = json.load(open(os.path.join(src, "metadata.json")))
    ascent = meta["max_ascent"]
    height = ascent + meta["max_descent"] + 1

    glyphs = {}
    with open(os.path.join(src, "glyphs.csv"), encoding="utf-8") as f:
        next(f)
        for line in f:
            p = [int(x) for x in line.strip().split(",")]
            glyphs[p[0]] = dict(adv=p[1], page=p[2], ox=p[3], oy=p[4],
                                x1=p[5], y1=p[6], x2=p[7], y2=p[8])

    pages = {}
    out = {}
    for ch in CHARS:
        g = glyphs.get(ord(ch))
        if not g:
            continue
        canvas = Image.new("RGBA", (max(1, g["adv"]), height), (0, 0, 0, 0))
        if g["page"] != 0xFFFF and g["x1"] != 0xFFFF:
            if g["page"] not in pages:
                pages[g["page"]] = Image.open(
                    os.path.join(src, "page%d.png" % g["page"])).convert("RGBA")
            ink = pages[g["page"]].crop((g["x1"], g["y1"], g["x2"], g["y2"]))
            canvas.alpha_composite(ink, (g["ox"], ascent - g["oy"]))
        buf = io.BytesIO()
        canvas.save(buf, format="PNG", optimize=True)
        b64 = base64.b64encode(buf.getvalue()).decode()
        out[ch] = {"w": canvas.width, "h": canvas.height,
                   "m": "data:image/png;base64," + b64}

    json.dump({"ascent": ascent, "height": height, "chars": out},
              open(out_path, "w"), separators=(",", ":"))
    print("wrote", out_path, os.path.getsize(out_path), "bytes,", len(out), "glyphs")

    # preview strip
    strip = Image.new("RGBA", (1000, height + 16), (16, 18, 28, 255))
    x = 4
    for ch in "0123456789 JUSTICE ATTACK MISS":
        g = out.get(ch)
        if not g:
            continue
        im = Image.open(io.BytesIO(base64.b64decode(g["m"].split(",", 1)[1])))
        strip.alpha_composite(im, (x, 8))
        x += im.width + 1
    strip.resize((strip.width * 2, strip.height * 2), Image.LANCZOS).save(
        os.path.join(os.path.dirname(out_path), "glyphs_preview.png"))


if __name__ == "__main__":
    main()
