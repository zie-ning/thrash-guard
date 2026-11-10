#!/usr/bin/env python3
"""tg_receiver.py가 남긴 원본 로그를 CSV로 바꾼다.

입력 줄: <recv_unix_ns>\\t<tg1 key=value ...>
출력 열: recv_unix_ns, 그다음 첫 샘플 줄에 나온 key 순서 그대로

원본 로그는 그대로 두고, 이 변환은 언제든 다시 할 수 있다.
"""

import argparse
import csv
import sys


def parse_line(line: str):
    recv, _, body = line.rstrip("\n").partition("\t")
    toks = body.split()
    if not toks or toks[0] != "tg1":
        return None
    row = {"recv_unix_ns": recv}
    for tok in toks[1:]:
        k, sep, v = tok.partition("=")
        if sep:
            row[k] = v
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", help="tg_receiver.py 출력 파일")
    ap.add_argument("--out", help="CSV 경로 (생략하면 표준 출력)")
    ap.add_argument("--kind", default="sample", help="이 kind의 줄만 변환 (기본 sample)")
    args = ap.parse_args()

    rows = []
    with open(args.log, encoding="utf-8") as f:
        for line in f:
            row = parse_line(line)
            if row is not None and row.get("kind") == args.kind:
                rows.append(row)
    if not rows:
        print("변환할 줄이 없다", file=sys.stderr)
        return 1

    # 샘플러가 나중에 key를 추가해도 열이 버려지지 않도록 모든 행의 key를 처음 나온 순서대로 모은다.
    fields = list(dict.fromkeys(k for row in rows for k in row))
    out = open(args.out, "w", newline="", encoding="utf-8") if args.out else sys.stdout
    w = csv.DictWriter(out, fieldnames=fields, extrasaction="ignore", lineterminator="\n")
    w.writeheader()
    w.writerows(rows)
    if args.out:
        out.close()
        print(f"{len(rows)}행 -> {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
