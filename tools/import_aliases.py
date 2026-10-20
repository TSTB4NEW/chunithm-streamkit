"""把社区别名合并成插件用的 music_alias.tsv。

来源：
  1. ChunithmUtil alias.json（cid 即歌曲 ID）
     https://github.com/AmethystTim/ChunithmUtil/blob/master/data/alias.json
  2. 落雪查分器 CHUNITHM 别名 API（song_id 即歌曲 ID）
     https://maimai.lxns.net/api/v0/chunithm/alias/list

用法：
  python tools/import_aliases.py --db <music_db.tsv> --out <music_alias.tsv>
  python tools/import_aliases.py --db ... --out ... --alias alias.json --skip-lxns

这两个来源都不是本项目资产，仅供本机个人使用；发布包不附带别名数据。
"""
import argparse
import io
import json
import os
import urllib.request

CHUNITHMUTIL_URL = "https://raw.githubusercontent.com/AmethystTim/ChunithmUtil/master/data/alias.json"
LXNS_URL = "https://maimai.lxns.net/api/v0/chunithm/alias/list"


def load_ids(path):
    ids = set()
    with io.open(path, "r", encoding="utf-8-sig") as f:
        for line in f:
            cols = line.rstrip("\n").split("\t")
            if len(cols) >= 3:
                try:
                    ids.add(int(cols[0]))
                except ValueError:
                    pass
    return ids


def fetch_json(url):
    print("fetch", url)
    with urllib.request.urlopen(url, timeout=30) as r:
        return json.loads(r.read().decode("utf-8"))


def as_id(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return -1


def add_aliases(by_id, song_id, aliases):
    sid = as_id(song_id)
    if sid < 0:
        return 0
    seen = by_id.setdefault(sid, [])
    known = {a.lower() for a in seen}
    n = 0
    for a in aliases:
        a = str(a).replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()
        if not a or a.lower() in known:
            continue
        seen.append(a)
        known.add(a.lower())
        n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", required=True, help="music_db.tsv")
    ap.add_argument("--out", required=True, help="music_alias.tsv output")
    ap.add_argument("--alias", help="本地 ChunithmUtil alias.json（不填则下载）")
    ap.add_argument("--skip-chunithmutil", action="store_true", help="不导入 ChunithmUtil")
    ap.add_argument("--skip-lxns", action="store_true", help="不导入落雪别名")
    args = ap.parse_args()

    ids = load_ids(args.db)
    by_id = {}
    stats = {}

    if not args.skip_chunithmutil:
        if args.alias:
            with io.open(args.alias, "r", encoding="utf-8") as f:
                data = json.load(f)
        else:
            data = fetch_json(CHUNITHMUTIL_URL)
        n = 0
        for entry in data.get("songs", []):
            cid = as_id(entry.get("cid"))
            if cid in ids:
                n += add_aliases(by_id, cid, entry.get("aliases", []))
        stats["chunithmutil"] = n

    if not args.skip_lxns:
        data = fetch_json(LXNS_URL)
        n = 0
        for entry in data.get("aliases", []):
            sid = as_id(entry.get("song_id"))
            if sid in ids:
                n += add_aliases(by_id, sid, entry.get("aliases", []))
        stats["lxns"] = n

    by_id = {k: v for k, v in by_id.items() if v}
    total = sum(len(v) for v in by_id.values())

    with io.open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# generated from community alias data (personal use)\n")
        for sid in sorted(by_id):
            f.write(str(sid) + "\t" + "\t".join(by_id[sid]) + "\n")

    print("songs in db:", len(ids))
    print("songs with aliases:", len(by_id))
    print("aliases written:", total)
    print("per source:", stats)
    print("output:", os.path.abspath(args.out))


if __name__ == "__main__":
    main()
