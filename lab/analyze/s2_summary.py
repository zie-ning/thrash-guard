#!/usr/bin/env python3
"""S2(컨테이너 내부 스래싱) 실행 결과를 판정한다 (M1-5, D12·D23).

S2는 컨테이너가 자기 메모리 상한에 걸려 스래싱하는 상황이라 시스템 전체는 멀쩡할 수 있다.
그래서 S1의 판정(SSH 무응답 + 시스템 MemAvailable 바닥) 대신 **카나리 응답**으로 판정한다.

판정:
  oom_kill         커널 OOM kill이 1건 이상 일어났다(컨테이너 상한에 걸린 cgroup OOM 포함)
  container_stall  OOM kill 0건, 카나리가 10분 이상 한 번도 응답하지 못했다
  normal           그 밖의 모든 경우
  running          (실행 중에만) 아직 확정되지 않았다

판정과 함께 피해 범위를 기록한다: 시스템 MemAvailable 최저, SSH 최장 무응답, 컨테이너 cgroup의
PSI full 비율(카나리 무응답 구간 기준), cgroup 이벤트(memory.events의 max·oom_kill) 증가량.

입력(결과 디렉터리 안): samples.log, ssh_probe.tsv, canary_probe.tsv
"""

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verdict as sv  # noqa: E402

STALL_GAP_S = 600
NS = 1_000_000_000
_KV = re.compile(r"(\w+)=(\S+)")


def load_cg(path):
    """kind=cg 줄에서 (수신 시각 ns, psi_full_total_us, ev_max, ev_oom_kill)만 뽑는다. 컨테이너 cgroup 하나만 본다."""
    out = []
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            recv, _, body = line.partition("\t")
            if " kind=cg " not in body:
                continue
            kv = dict(_KV.findall(body))
            if "container" not in kv.get("name", ""):
                continue
            try:
                out.append((int(recv), int(kv["psi_full_total"]), int(kv["ev_max"]), int(kv["ev_oom_kill"])))
            except (KeyError, ValueError):
                continue
    return out


def evaluate(d, now_ns=None, final=False):
    ssh = sv.load_probes(os.path.join(d, "ssh_probe.tsv"))
    canary = sv.load_probes(os.path.join(d, "canary_probe.tsv"))
    samples = sv.load_samples(os.path.join(d, "samples.log"))
    cg = load_cg(os.path.join(d, "samples.log"))

    if now_ns is None:
        now_ns = max([t for t, _ in canary] + [t for t, _ in ssh] + [t for t, _, _ in samples] + [0])

    gap_ns, gap_start, gap_end = sv.longest_gap(canary, now_ns)
    ssh_gap_ns = sv.longest_gap(ssh, now_ns)[0]

    oom_delta = 0
    if samples:
        oom_delta = max(o for _, _, o in samples) - min(o for _, _, o in samples)
    mem_min = min((m for _, m, _ in samples), default=None)

    cg_oom_delta = (max(c[3] for c in cg) - min(c[3] for c in cg)) if cg else 0
    cg_max_delta = (max(c[2] for c in cg) - min(c[2] for c in cg)) if cg else 0

    full_ratio = None
    in_gap = [c for c in cg if gap_start <= c[0] <= gap_end]
    if len(in_gap) >= 2 and in_gap[-1][0] > in_gap[0][0]:
        # PSI total은 마이크로초, 수신 시각은 나노초다. 같은 단위(밀리초)로 맞춰 비율을 만든다.
        full_ms = (in_gap[-1][1] - in_gap[0][1]) / 1000.0
        span_ms = (in_gap[-1][0] - in_gap[0][0]) / 1e6
        full_ratio = round(full_ms / span_ms, 3)

    gap_s = gap_ns / NS
    if oom_delta > 0:
        verdict = "oom_kill"
    elif gap_s >= STALL_GAP_S:
        verdict = "container_stall"
    else:
        verdict = "normal" if final else "running"

    return {
        "verdict": verdict,
        "canary_max_gap_s": round(gap_s, 1),
        "canary_probes": len(canary),
        "canary_ok": sum(1 for _, ok in canary if ok),
        "ssh_max_gap_s": round(ssh_gap_ns / NS, 1),
        "mem_avail_min_mib": None if mem_min is None else round(mem_min / 1024, 1),
        "cg_psi_full_ratio_in_gap": full_ratio,
        "cg_ev_max_delta": cg_max_delta,
        "cg_ev_oom_kill_delta": cg_oom_delta,
        "oom_kill_delta": oom_delta,
        "samples": len(samples),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dir")
    ap.add_argument("--final", action="store_true")
    ap.add_argument("--now-ns", type=int)
    ap.add_argument("--field")
    args = ap.parse_args()
    res = evaluate(args.dir, args.now_ns, args.final)
    print(res[args.field] if args.field else json.dumps(res, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
