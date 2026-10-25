"""Slice the per-judgement COUNT digits from CHU_UI_Result_score_NUM_00.dds.

The game colours each judge row's count to match its label:
  JC  -> gold gradient (top) to cyan (bottom)
  JU  -> orange
  AT  -> green
  MISS-> silver
plus a plain white set used by the LATE/FAST totals.

Source rects come from CHU_UI_Result_01_v10.srd (TEX CHU_UI_Result_score_NUM_00).
Emits web/judge_digits.json: { set: { char: {w,h,m} } }
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

SRD = r"X:\Game\Chunithm\SDHD - MATE\data\surfboard\play\result\CHU_UI_Result_01_v10.srd"
TEX = r"X:\Game\Chunithm\SDHD - MATE\data\surfboard\texture\CHU_UI_Result_score_NUM_00.dds"
OUT = r"X:\杂物\文档\deepseek-harness\default-workspace\chunithm-streamkit\web\judge_digits.json"

# rect index of digit "0" for each set (10 digits follow, row-major)
SETS = {"jc": 11, "ju": 21, "at": 31, "mi": 41, "white": 0}


def main():
    rects = None
    for name, rs, off in sections(open(SRD, "rb").read()):
        if name.split("@")[0] == "CHU_UI_Result_score_NUM_00":
            rects = rs
            break
    if not rects:
        raise SystemExit("score_NUM_00 section not found")
    img = read_dds(TEX).convert("RGBA")

    out = {}
    for key, start in SETS.items():
        chars = {}
        for i, ch in enumerate("0123456789"):
            r = rects[start + i]
            crop = img.crop((r[0], r[1], r[2], r[3]))
            buf = io.BytesIO()
            crop.save(buf, "PNG", optimize=True)
            chars[ch] = {"w": crop.width, "h": crop.height,
                         "m": "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()}
        out[key] = chars
        print("set %-5s %d chars, cell %dx%d" % (key, len(chars), crop.width, crop.height))

    json.dump(out, open(OUT, "w"), separators=(",", ":"))
    print("wrote", OUT, os.path.getsize(OUT))


if __name__ == "__main__":
    main()
