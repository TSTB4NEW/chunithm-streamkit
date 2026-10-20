"""用本地 Music.xml 给 music_db.tsv 补充分类/版本列。

只读游戏文件，不联网。输出列：
  id title artist bpm basic advanced expert master ultima we genre version

用法：
  python tools/local_metadata.py --game-root "<游戏目录>" \
      --db <music_db.tsv> --out <music_db.tsv>
"""
import argparse
import io
import os
import re
import xml.etree.ElementTree as ET

MUSIC_DIR = re.compile(r"music(\d+)$")

# releaseTagName → 游戏版本
RELEASE_VERSION = {
    "v1 1.00.00": "CHUNITHM",
    "v1 1.05.00": "PLUS",
    "v1 1.10.00": "AIR",
    "v1 1.15.00": "AIR PLUS",
    "v1 1.20.00": "STAR",
    "v1 1.25.00": "STAR PLUS",
    "v1 1.30.00": "AMAZON",
    "v1 1.35.00": "AMAZON PLUS",
    "v1 1.40.00": "CRYSTAL",
    "v1 1.45.00": "CRYSTAL PLUS",
    "v1 1.50.00": "PARADISE",
    "v1 1.55.00": "PARADISE LOST",
    "v2 2.00.00": "NEW",
    "v2 2.05.00": "NEW PLUS",
    "v2 2.10.00": "SUN",
    "v2 2.15.00": "SUN PLUS",
    "v2 2.20.00": "LUMINOUS",
    "v2 2.25.00": "LUMINOUS PLUS",
    "v2 2.30.00": "VERSE",
    "v2 2.35.00": "X-VERSE",
    "v2 2.40.00": "X-VERSE",
    "v2 2.45.00": "X-VERSE-X",
    "v2 2.50.00": "MATE",
}

GENRE_NAME = {
    "ORIGINAL": "原创",
    "イロドリミドリ": "彩绿",
    "東方Project": "东方Project",
    "东方Project": "东方Project",
    "GAME": "其他游戏",
}


def find_xml_roots(game_root):
    roots = [os.path.join(game_root, "data")]
    opt = os.path.join(game_root, "bin", "option")
    if os.path.isdir(opt):
        roots.append(opt)
    return roots


def parse_song(path):
    try:
        tree = ET.parse(path)
    except ET.ParseError:
        return None
    root = tree.getroot()

    def text(tag):
        el = root.find(".//" + tag + "/str")
        if el is not None and el.text:
            return el.text.strip()
        el = root.find(".//" + tag)
        if el is not None and el.text:
            return el.text.strip()
        return ""

    genre = ""
    for el in root.findall(".//genreNames/list/StringID/str"):
        if el.text and el.text.strip():
            genre = el.text.strip()
            break
    genre = GENRE_NAME.get(genre, genre)

    release = text("releaseTagName")
    works = text("worksName")
    version = RELEASE_VERSION.get(release, "")
    if not version and "Ver." in works:
        version = works.split("Ver.")[-1].strip()

    we_tag = text("worldsEndTagName")
    if we_tag == "Invalid":
        we_tag = ""
    return {"genre": genre, "version": version, "we_tag": we_tag}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-root", required=True)
    ap.add_argument("--db", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    meta = {}
    for root in find_xml_roots(args.game_root):
        for dirpath, _, files in os.walk(root):
            m = MUSIC_DIR.search(os.path.basename(dirpath))
            if not m or "Music.xml" not in files:
                continue
            sid = int(m.group(1))
            info = parse_song(os.path.join(dirpath, "Music.xml"))
            if info:
                meta[sid] = info   # option 后遍历，覆盖 data

    rows = []
    used = 0
    with io.open(args.db, "r", encoding="utf-8-sig") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) < 3:
                continue
            cols = (cols + [""] * 13)[:13]
            try:
                sid = int(cols[0])
            except ValueError:
                continue
            info = meta.get(sid, {})
            cols[10] = info.get("we_tag", "")
            cols[11] = info.get("genre", "")
            cols[12] = info.get("version", "")
            if info:
                used += 1
            rows.append(cols)

    with io.open(args.out, "w", encoding="utf-8", newline="\n") as f:
        for cols in rows:
            f.write("\t".join(cols) + "\n")

    print("local songs:", len(meta))
    print("db rows:", len(rows))
    print("rows with local meta:", used)
    print("output:", os.path.abspath(args.out))


if __name__ == "__main__":
    main()
