"""Cut digits/symbols out of an unpacked RFZ atlas into per-glyph PNGs whose
canvas equals the glyph advance (so they tile with correct side bearings).

Usage: python rfz_digits.py <rfz_out_dir> <web_font_dir>
"""
import json
import os
import sys

import numpy as np
from PIL import Image

CHARS = ("0123456789" "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
         " /:,.+-%")
SPECIAL_BLANK = {" "}


def main():
    src = sys.argv[1]
    dst = sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    meta = json.load(open(os.path.join(src, "metadata.json")))
    ascent = meta["max_ascent"]
    height = ascent + meta["max_descent"] + 1

    glyphs = {}
    with open(os.path.join(src, "glyphs.csv"), encoding="utf-8") as f:
        next(f)
        for line in f:
            p = [int(x) for x in line.strip().split(",")]
            glyphs[p[0]] = {
                "adv": p[1], "page": p[2], "ox": p[3], "oy": p[4],
                "x1": p[5], "y1": p[6], "x2": p[7], "y2": p[8],
            }

    pages = {}
    out = {}
    for ch in CHARS:
        g = glyphs.get(ord(ch))
        if not g:
            continue
        if g["page"] == 0xFFFF or g["x1"] == 0xFFFF:
            canvas = Image.new("RGBA", (max(1, g["adv"]), height), (0, 0, 0, 0))
            name = "d%s.png" % ord(ch)
            canvas.save(os.path.join(dst, name))
            out[ch] = {"file": name, "w": canvas.width, "h": canvas.height}
            continue
        if g["page"] not in pages:
            pages[g["page"]] = Image.open(os.path.join(src, "page%d.png" % g["page"])).convert("RGBA")
        atlas = pages[g["page"]]
        ink = atlas.crop((g["x1"], g["y1"], g["x2"], g["y2"]))
        canvas = Image.new("RGBA", (max(1, g["adv"]), height), (0, 0, 0, 0))
        canvas.alpha_composite(ink, (g["ox"], ascent - g["oy"]))
        name = "d%s.png" % (ord(ch))
        canvas.save(os.path.join(dst, name))
        out[ch] = {"file": name, "w": canvas.width, "h": canvas.height}
        print("  %r -> %s %dx%d" % (ch, name, canvas.width, canvas.height))

    json.dump({"ascent": ascent, "height": height, "chars": out},
              open(os.path.join(dst, "digits.json"), "w"), indent=1)
    # preview strip on dark background
    strip = Image.new("RGBA", (900, height + 20), (16, 18, 28, 255))
    x = 4
    for ch in "0123456789/-%":
        g = out.get(ch)
        if not g:
            continue
        im = Image.open(os.path.join(dst, g["file"]))
        strip.alpha_composite(im, (x, 10))
        x += im.width + 2
    strip.resize((strip.width * 2, strip.height * 2), Image.LANCZOS).save(
        os.path.join(dst, "preview.png"))


if __name__ == "__main__":
    main()
