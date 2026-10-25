"""Measure a game reference card against our rendered card.

Usage:
  python card_diff.py <reference.png> <ours.png> [out.png]

Both images are scaled to the same width, then:
  * tab      : pink/orange gradient region (top-left)
  * palebar  : pale yellow bar
  * digits   : ink bounding box of the metallic numbers
are reported as ratios of the card width so the two can be compared
even at different scales.
"""
import sys

import numpy as np
from PIL import Image


def mask_tab(a):
    r, g, b = a[..., 0].astype(int), a[..., 1].astype(int), a[..., 2].astype(int)
    return (r > 220) & (g > 60) & (g < 190) & (b < 140)


def mask_bar(a):
    r, g, b = a[..., 0].astype(int), a[..., 1].astype(int), a[..., 2].astype(int)
    return (r > 235) & (g > 225) & (g < 250) & (b > 190) & (b < 235)


def mask_ink(a):
    r, g, b = a[..., 0].astype(int), a[..., 1].astype(int), a[..., 2].astype(int)
    grey = (abs(r - g) < 18) & (abs(g - b) < 18)
    return grey & (r > 120) & (r < 235)


def measure(path, w=None):
    im = Image.open(path).convert("RGB")
    if w:
        im = im.resize((w, int(im.height * w / im.width)), Image.LANCZOS)
    a = np.array(im)
    H, W = a.shape[:2]
    out = {"size": (W, H)}

    for name, fn in (("tab", mask_tab), ("bar", mask_bar), ("ink", mask_ink)):
        m = fn(a)
        ys, xs = np.nonzero(m)
        if len(ys) == 0:
            out[name] = None
            continue
        out[name] = dict(x0=xs.min() / W, x1=xs.max() / W,
                         y0=ys.min() / H, y1=ys.max() / H,
                         h=(ys.max() - ys.min() + 1) / H,
                         w=(xs.max() - xs.min() + 1) / W)
    return im, out


def main():
    ref, ours = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else None
    im_r, m_r = measure(ref, w=600)
    im_o, m_o = measure(ours, w=600)
    for k in ("tab", "bar", "ink"):
        r, o = m_r[k], m_o[k]
        print("== %s ==" % k)
        if not r or not o:
            print("   ref", r, " ours", o)
            continue
        for f in ("y0", "y1", "h", "w", "x0"):
            print("   %-3s ref %.3f  ours %.3f  diff %+.3f" % (f, r[f], o[f], o[f] - r[f]))
    if out:
        h = max(im_r.height, im_o.height)
        canvas = Image.new("RGB", (im_r.width + im_o.width + 20, h), (22, 22, 28))
        canvas.paste(im_r, (0, 0))
        canvas.paste(im_o, (im_r.width + 20, 0))
        canvas.save(out)
        print("wrote", out)


if __name__ == "__main__":
    main()
