"""清理历史日志里的敏感字段（房间 token / WS key / buvid / cookie）。

用法：
  python tools/scrub_log.py <log 文件> [更多日志...]

只做本机文本脱敏，不联网。
"""
import io
import os
import re
import sys

PATTERNS = [
    (re.compile(r'("(?:key|token|buvid|SESSDATA)"\s*:\s*")[^"]*(")', re.I), r'\1<redacted>\2'),
    (re.compile(r'(dm: auth=).*'), r'\1<redacted>'),
    (re.compile(r'(dm: info=).*'), r'\1<redacted>'),
    (re.compile(r'(dm: auth reply ).*'), r'\1received'),
]


def scrub(path):
    with io.open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    orig = text
    for rx, repl in PATTERNS:
        text = rx.sub(repl, text)
    if text != orig:
        tmp = path + ".tmp"
        with io.open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(tmp, path)
        print("scrubbed:", path)
    else:
        print("clean:", path)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: scrub_log.py <log> [...]")
        sys.exit(2)
    for p in sys.argv[1:]:
        scrub(p)
