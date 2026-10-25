"""Build a labelled contact sheet of selected DDS textures for quick visual search."""
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dds2png import read_dds

TEXDIR = r"X:\Game\Chunithm\SDHD - MATE\data\surfboard\texture"
OUT = r"X:\杂物\文档\deepseek-harness\default-workspace\chunithm-streamkit\.tmp_ui\sheet.png"


def main():
    names = sys.argv[1:]
    tiles = []
    for n in names:
        p = os.path.join(TEXDIR, n + ".dds")
        try:
            img = read_dds(p)
        except Exception as e:
            print("skip", n, e)
            continue
        img.thumbnail((360, 360))
        tiles.append((n, img))
    if not tiles:
        raise SystemExit("nothing to show")
    cols = 3
    rows = (len(tiles) + cols - 1) // cols
    cell_w, cell_h = 380, 400
    sheet = Image.new("RGB", (cols * cell_w, rows * cell_h), (24, 26, 34))
    d = ImageDraw.Draw(sheet)
    for i, (name, img) in enumerate(tiles):
        cx, cy = (i % cols) * cell_w, (i // cols) * cell_h
        sheet.paste(img, (cx + 10, cy + 30), img if img.mode == "RGBA" else None)
        d.text((cx + 10, cy + 8), name, fill=(230, 230, 240))
    sheet.save(OUT)
    print("wrote", OUT, sheet.size, len(tiles), "tiles")


if __name__ == "__main__":
    main()
