#!/usr/bin/env python3
"""탐색 결과 디렉터리 아래의 result.json을 모아 한 장의 표로 만든다 (M1-3 재현 지도).

사용법: summary.py <디렉터리>   (하위 디렉터리를 모두 찾아 읽는다)
출력: 마크다운 표. 부하 조건 순서로 정렬한다.
"""

import json
import os
import sys


def collect(root):
    rows = []
    for dirpath, _, files in os.walk(root):
        if "result.json" not in files:
            continue
        try:
            with open(os.path.join(dirpath, "result.json"), encoding="utf-8") as f:
                r = json.load(f)
        except (OSError, ValueError):
            continue
        load = r.get("load", {})
        res = r.get("result", {})
        rows.append({
            "dir": os.path.relpath(dirpath, root),
            "anon": load.get("anon_mib"),
            "file": load.get("file_mib"),
            "rate": load.get("rate_mib_s"),
            "pin": load.get("pin_mib", 0),
            "iops": (r.get("io_limit") or {}).get("iops"),
            "verdict": res.get("verdict"),
            "gap": res.get("ssh_max_gap_s"),
            "mem": res.get("mem_avail_min_mib"),
            "oom": res.get("oom_kill_delta"),
        })
    rows.sort(key=lambda x: (x["anon"] or 0, x["file"] or 0, x["pin"] or 0, x["iops"] or 0, x["rate"] or 0, x["dir"]))
    return rows


def render(rows):
    lines = ["| anon | file | pin | IOPS | 속도 | 판정 | SSH 최장 무응답(초) | MemAvailable 최소(MiB) | OOM kill | 실행 |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        lines.append(f"| {r['anon']} | {r['file']} | {r['pin']} | {r['iops']} | {r['rate']} | {r['verdict']} | {r['gap']} | "
                     f"{r['mem']} | {r['oom']} | {r['dir']} |")
    counts = {}
    for r in rows:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    lines.append("")
    lines.append("합계: " + ", ".join(f"{k} {v}회" for k, v in sorted(counts.items())) if rows
                 else "결과 없음")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    print(render(collect(sys.argv[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
