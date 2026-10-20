#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
chunithm-streamkit 歌曲库生成工具（离线，只读游戏数据文件）。

用法：
    python build_music_db.py <music.xml 路径> [输出路径]

输出：Tab 分隔 UTF-8 TSV（无表头，带 BOM，Excel 可直接打开）：
    id  title  artist  bpm  basic  advanced  expert  master  ultima  worlds_end
后 6 列为各难度定数，未知留空。

说明：不同版本的 music.xml 结构略有差异；本工具采用宽松解析：
  * 每个 <MusicData ...> ... </MusicData> 块为一首歌；
  * <id> 取块内第一个整数；
  * <name>/<artist>/<artistName>/<bpm> 支持嵌套 <str> 形式；
  * 难度定数：块内每个 <difficulty> 子块中 <str> 难度名 + 第一个 <const> 数值。
若你的版本字段不同，改下方 EXTRACT/parse_block 即可（插件端 songdb.h 逻辑一致）。
"""
import re
import sys
import os

DIFFS = ["BASIC", "ADVANCED", "EXPERT", "MASTER", "ULTIMA", "WORLD'S END"]


def extract_field(block: str, tag: str) -> str:
    m = re.search(r"<" + tag + r"[^>]*>(.*?)</" + tag + r">", block, re.S)
    if not m:
        return ""
    inner = m.group(1)
    sm = re.search(r"<str[^>]*>(.*?)</str>", inner, re.S)
    if sm:
        inner = sm.group(1)
    inner = re.sub(r"<[^>]+>", "", inner)
    return inner.strip()


def extract_num(block: str, tag: str) -> float:
    v = extract_field(block, tag)
    try:
        return float(v)
    except ValueError:
        return 0.0


def parse_block(block: str):
    sid = extract_field(block, "id")
    try:
        mid = int(sid)
    except ValueError:
        return None
    if mid <= 0 or mid > 0xFFFFFFFE:
        return None
    title = extract_field(block, "name")
    artist = extract_field(block, "artist") or extract_field(block, "artistName")
    bpm = extract_num(block, "bpm")
    consts = [0.0] * 6
    for dm in re.finditer(r"<difficulty[^>]*>(.*?)</difficulty>", block, re.S):
        sub = dm.group(1)
        name = extract_field(sub, "str")
        c = extract_num(sub, "const")
        for i, d in enumerate(DIFFS):
            if name.upper() == d:
                consts[i] = c
    if not title:
        return None
    return mid, title, artist, bpm, consts


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "music_db.tsv"
    with open(src, "rb") as f:
        raw = f.read()
    xml = raw.decode("utf-8", "replace")

    songs = []
    for m in re.finditer(r"<MusicData[^>]*>.*?</MusicData>", xml, re.S):
        r = parse_block(m.group(0))
        if r:
            songs.append(r)
    songs.sort(key=lambda x: x[0])

    with open(out, "w", encoding="utf-8-sig", newline="\r\n") as f:
        for mid, title, artist, bpm, consts in songs:
            row = [str(mid), title, artist, f"{bpm:.2f}"]
            row += [f"{c:.2f}" if c > 0 else "" for c in consts]
            f.write("\t".join(row) + "\r\n")
    print(f"parsed {len(songs)} songs -> {os.path.abspath(out)}")


if __name__ == "__main__":
    main()
