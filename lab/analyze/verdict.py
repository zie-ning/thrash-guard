#!/usr/bin/env python3
"""실행 결과 디렉터리에서 재현 판정을 계산한다 (M1-2).

하네스가 실행 중에 주기적으로 호출해 "끝내도 되는가"를 정하고, 실행이 끝난 뒤에는 같은 코드로
최종 판정을 다시 계산한다. 판단 기준이 한 곳에만 있어 두 결과가 어긋나지 않는다.

판정(세 갈래):
  oom_kill  커널 OOM kill이 1건 이상 일어났다
  livelock  OOM kill 0건, SSH가 10분 이상 한 번도 성공하지 못했고, 그 구간에서 MemAvailable이
            20 MiB 이하까지 내려갔다 (plan.md 재현 합격 기준)
  normal    그 밖의 모든 경우. 경계 조건(예: 9분 59초 무응답)도 여기에 들어가므로
            경계 판단에 쓸 수 있도록 SSH 최장 무응답 시간 등을 함께 기록한다
  running   (실행 중에만) 아직 위 둘 중 어느 쪽도 확정되지 않았다

입력(결과 디렉터리 안):
  samples.log    tg_receiver.py 원본 로그 (<recv_unix_ns>\\t<tg1 key=value ...>)
  ssh_probe.tsv  ssh_probe.py 출력
"""

import argparse
import json
import os
import re
import sys

LIVELOCK_GAP_S = 600           # SSH 무응답 기준: 성공이 한 번도 없는 구간의 최소 길이
MEM_AVAIL_LIMIT_KB = 20 * 1024  # MemAvailable 기준: 20 MiB
NS = 1_000_000_000

_KV = re.compile(r"(\w+)=(\S+)")


def load_probes(path):
    """(완료 시각 ns, 성공 여부) 목록. 프로브는 시작 시각 기준으로 기록되므로 지연을 더해 완료 시각을 만든다."""
    out = []
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        next(f, None)  # 헤더
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            try:
                start_ns = int(parts[0])
                latency_ms = float(parts[1])
                ok = parts[2] == "1"
            except ValueError:
                continue  # 쓰는 도중 잘린 줄
            out.append((start_ns + int(latency_ms * 1e6), ok))
    out.sort()
    return out


def load_samples(path):
    """kind=sample 줄에서 (수신 시각 ns, mem_avail_kb, oom_kill)만 뽑는다."""
    out = []
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            recv, _, body = line.partition("\t")
            if " kind=sample " not in body:
                continue
            kv = dict(_KV.findall(body))
            try:
                out.append((int(recv), int(kv["mem_avail_kb"]), int(kv["oom_kill"])))
            except (KeyError, ValueError):
                continue  # 쓰는 도중 잘린 줄
    return out


def longest_gap(probes, end_ns):
    """성공이 없는 가장 긴 구간 (길이 ns, 시작 ns, 끝 ns).

    구간은 연속한 두 성공 사이와, 마지막 성공부터 end_ns까지다. 성공이 하나도 없으면 첫 프로브부터다.
    """
    oks = [t for t, ok in probes if ok]
    if not probes:
        return 0, 0, 0
    points = oks if oks else [probes[0][0]]
    best = (0, points[0], points[0])
    for a, b in zip(points, points[1:]):
        if b - a > best[0]:
            best = (b - a, a, b)
    if end_ns - points[-1] > best[0]:
        best = (end_ns - points[-1], points[-1], end_ns)
    return best


def evaluate(d, now_ns=None, final=False):
    probes = load_probes(os.path.join(d, "ssh_probe.tsv"))
    samples = load_samples(os.path.join(d, "samples.log"))

    if now_ns is None:
        now_ns = max([t for t, _ in probes] + [t for t, _, _ in samples] + [0])

    gap_ns, gap_start, gap_end = longest_gap(probes, now_ns)

    oom_delta = 0
    if samples:
        oom_delta = max(o for _, _, o in samples) - min(o for _, _, o in samples)

    in_gap = [m for t, m, _ in samples if gap_start <= t <= gap_end]
    mem_min_gap = min(in_gap) if in_gap else None
    mem_min_all = min((m for _, m, _ in samples), default=None)

    gap_s = gap_ns / NS
    livelock = (oom_delta == 0 and gap_s >= LIVELOCK_GAP_S and
                mem_min_gap is not None and mem_min_gap <= MEM_AVAIL_LIMIT_KB)

    if oom_delta > 0:
        verdict = "oom_kill"
    elif livelock:
        verdict = "livelock"
    else:
        verdict = "normal" if final else "running"

    def mib(kb):
        return None if kb is None else round(kb / 1024, 1)

    return {
        "verdict": verdict,
        "ssh_max_gap_s": round(gap_s, 1),
        "ssh_probes": len(probes),
        "ssh_ok": sum(1 for _, ok in probes if ok),
        "mem_avail_min_mib": mib(mem_min_all),
        "mem_avail_min_mib_in_gap": mib(mem_min_gap),
        "oom_kill_delta": oom_delta,
        "samples": len(samples),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dir", help="결과 디렉터리")
    ap.add_argument("--final", action="store_true", help="최종 판정 (running을 normal로 확정)")
    ap.add_argument("--now-ns", type=int, help="현재 시각(유닉스 ns). 생략하면 기록 중 가장 늦은 시각")
    ap.add_argument("--field", help="이 필드의 값만 출력")
    args = ap.parse_args()

    res = evaluate(args.dir, args.now_ns, args.final)
    if args.field:
        print(res[args.field])
    else:
        print(json.dumps(res, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
