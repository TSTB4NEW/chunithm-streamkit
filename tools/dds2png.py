"""Convert game .dds UI textures to PNG for inspection (BC1/2/3 + uncompressed)."""
import os
import sys

import numpy as np
from PIL import Image


def read_dds(path):
    d = open(path, "rb").read()
    assert d[:4] == b"DDS ", path
    h = 128
    height, width = int.from_bytes(d[12:16], "little"), int.from_bytes(d[16:20], "little")
    fourcc = d[84:88]
    bitcount = int.from_bytes(d[88:90], "little")
    data = d[h:]
    if fourcc == b"DXT1":
        img = Image.frombytes("RGBA", (width, height), data[: width * height // 2], "bcn", 1)
    elif fourcc == b"DXT5":
        img = Image.frombytes("RGBA", (width, height), data[: width * height], "bcn", 3)
    elif fourcc == b"DXT3":
        img = Image.frombytes("RGBA", (width, height), data[: width * height], "bcn", 2)
    elif fourcc == b"\0\0\0\0" and bitcount == 32:
        a = np.frombuffer(data[: width * height * 4], dtype=np.uint8).reshape(height, width, 4)
        img = Image.fromarray(a[..., [2, 1, 0, 3]], "RGBA")
    else:
        raise SystemExit("unsupported dds %s fourcc=%r bits=%d" % (path, fourcc, bitcount))
    return img


def main():
    src, out = sys.argv[1], sys.argv[2]
    img = read_dds(src)
    img.save(out)
    print("%s %dx%d -> %s" % (os.path.basename(src), img.width, img.height, out))


if __name__ == "__main__":
    main()
