#!/usr/bin/env python3
"""M4 효과 검증 실행 1회의 결과를 계산한다 (정의: docs/validation/measurement.md).

하네스(lab/m4/run.sh)가 실행 중에 주기적으로 --field status로 "끝내도 되는가"를 묻고,
실행이 끝난 뒤에는 같은 코드로 최종 결과(result.json)를 만든다. 기준이 한 곳에만 있어 두 결과가 어긋나지 않는다.

계산하는 값 (시각은 모두 VM 밖 수신기 시계, 결과는 부하 시작 기준 초):
  T0         시스템 PSI full의 1초 구간 비율이 10% 이상인 상태가 처음 3초 연속된 구간의 시작
  T_resolve  T0 이후, 비율이 10% 미만인 상태가 처음 10초 연속된 구간의 **끝**
             (조건이 충족된 시점. 그래서 종료 즉시 회복해도 해소 시간에는 최소 10초가 더해진다)
  해소 시간  T_resolve - T0. T0 + cap(기본 300초) 안에 T_resolve가 없으면 unresolved
  T_detect   데몬 판단 로그의 첫 `judge kill` (dry-run 포함)
  T_kill     실제 종료(dry_run=0, result=0)의 `judge kill`
  T_oom      커널 oom_kill 카운터가 처음 늘어난 샘플의 수신 시각

PSI 비율은 누적값 total의 차분으로 계산한다(avg10은 짧은 stall을 놓친다).
일련번호(seq)가 건너뛴 샘플 구간이 겹치는 1초 구간은 무효로 보고, 무효 구간은 3초·10초 연속 조건을 끊는다.

판정(status):
  running     아직 확정되지 않았다 (실행 중에만)
  resolved    T0 이후 cap 안에 해소되었다
  unresolved  T0 이후 cap까지 해소되지 않았다
  truncated   부하가 자체 종료(--duration)되어 cap 전에 창이 잘렸다. 해소 시간을 주장하지 않는다
  no_thrash   관찰 시간(기본 부하 시작 후 600초, 정상 버스트는 observe_sec) 안에 T0가 없었다
  incomplete  (최종 계산에서만) 실행이 확정 전에 끝났다

입력(결과 디렉터리 안):
  run.json            실행 조건. load_start_unix_ns, load_sec, cap_sec, observe_sec, guest_clock_offset_ns 등
  samples.log         tg_receiver.py 원본 로그
  ssh_probe.tsv       ssh_probe.py 출력
  canary_probe.tsv    canary_probe.py 출력 (필수 프로세스 E)
  daemon.log          VM 안 데몬 로그(journalctl -o cat). 없을 수 있다
  dmesg_oom.txt       VM 안 dmesg의 OOM 줄. 없을 수 있다
  e_state.txt         종료 시점의 E(tg-canary) 서비스 상태. 없을 수 있다
"""

import argparse
import bisect
import datetime
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verdict as sv  # noqa: E402  (load_probes를 재사용한다)

NS = 1_000_000_000
US_PER_S = 1_000_000
FULL_RATIO = 0.10          # T0, T_resolve의 10% 기준 (관찰 기준이며 데몬 임계와 별개)
T0_RUN_S = 3               # T0: 10% 이상이 이만큼 연속
RESOLVE_RUN_S = 10         # T_resolve: 10% 미만이 이만큼 연속
DEFAULT_CAP_S = 300        # 해소를 기다리는 시간 (T0 이후)
DEFAULT_NO_THRASH_S = 600  # T0를 기다리는 시간 (부하 시작 이후)

_KV = re.compile(r"(\w+)=(\S+)")
_DAEMON_LINE = re.compile(r"^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?)Z\s+(\w+)\s+(.*)$")

# 데몬 로그의 cgroup 이름과 역할. target_match와 하네스의 서비스 이름이 같아야 한다.
ROLES = {"tg-load.service": "load", "tg-canary.service": "E"}


# ---------- 입력 읽기 ----------

def load_samples(path):
    """kind=sample 줄에서 PSI full 누적값 등을 뽑는다. 쓰다 만 줄은 건너뛴다."""
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
                out.append({
                    "recv_ns": int(recv), "seq": int(kv["seq"]), "mono_us": int(kv["mono_us"]),
                    "full_us": int(kv["psi_full_total"]),
                    "avail_kb": int(kv["mem_avail_kb"]), "oom_kill": int(kv["oom_kill"]),
                })
            except (KeyError, ValueError):
                continue
    out.sort(key=lambda s: s["mono_us"])
    return out


def load_run(d):
    path = os.path.join(d, "run.json")
    if not os.path.exists(path):
        return {}
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def _read_text(path):
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


# ---------- 1초 구간 ----------

def build_buckets(samples):
    """guest 단조 시계 기준 1초 구간 목록. 각 항목: start_ns, end_ns(수신 시계), ratio(None이면 무효).

    구간의 PSI full 비율은 경계 시각의 누적값을 인접 샘플 사이에서 선형 보간해 차분한다.
    구간과 겹치는 샘플 구간 가운데 일련번호가 건너뛴 것이 있으면 ratio를 None으로 둔다.
    """
    n = len(samples)
    if n < 2:
        return []
    mono = [s["mono_us"] for s in samples]
    recv = [s["recv_ns"] for s in samples]
    full = [s["full_us"] for s in samples]
    bad = [samples[i + 1]["seq"] != samples[i]["seq"] + 1 or mono[i + 1] <= mono[i] for i in range(n - 1)]

    def at(values, x, i):
        span = mono[i + 1] - mono[i]
        if span <= 0:
            return values[i]
        return values[i] + (values[i + 1] - values[i]) * (x - mono[i]) / span

    buckets = []
    k = 0
    while True:
        b0 = mono[0] + k * US_PER_S
        b1 = b0 + US_PER_S
        if b1 > mono[-1]:
            break
        i0 = max(0, min(bisect.bisect_right(mono, b0) - 1, n - 2))
        i1 = max(0, min(bisect.bisect_left(mono, b1) - 1, n - 2))
        valid = not any(bad[i0:i1 + 1])
        ratio = None
        if valid:
            ratio = (at(full, b1, i1) - at(full, b0, i0)) / US_PER_S
        buckets.append({
            "start_ns": int(at(recv, b0, i0)), "end_ns": int(at(recv, b1, i1)), "ratio": ratio,
        })
        k += 1
    return buckets


def find_run(buckets, start_idx, pred, need):
    """start_idx 이후 pred를 만족하는 구간이 need개 연속된 첫 위치 (첫 인덱스, 마지막 인덱스). 무효 구간은 연속을 끊는다."""
    run = 0
    for idx in range(start_idx, len(buckets)):
        r = buckets[idx]["ratio"]
        if r is not None and pred(r):
            run += 1
            if run == need:
                return idx - need + 1, idx
        else:
            run = 0
    return None


# ---------- 프로브 ----------

def window_gap(probes, win_start, win_end):
    """창 안의 E 최대 무응답(초)과 성공률. 프로브는 (완료 시각 ns, 성공 여부). 창 밖의 무응답은 자른다."""
    if win_end <= win_start:
        return {"max_gap_s": None, "ok_rate": None, "probes": 0}
    oks = [t for t, ok in probes if ok and win_start <= t <= win_end]
    pts = [win_start] + oks + [win_end]
    gap = max(b - a for a, b in zip(pts, pts[1:]))
    in_win = [ok for t, ok in probes if win_start <= t <= win_end]
    rate = round(sum(1 for o in in_win if o) / len(in_win), 3) if in_win else None
    return {"max_gap_s": round(gap / NS, 1), "ok_rate": rate, "probes": len(in_win)}


# ---------- 데몬 로그 ----------

def parse_daemon_log(text, offset_ns):
    """데몬 로그에서 판단 기록을 뽑는다. 시각은 guest 벽시계라 offset(guest - host)을 빼 수신기 시계로 맞춘다."""
    events = {"kills": [], "cands": [], "start_mode": None}
    if not text:
        return events
    epoch = datetime.datetime(1970, 1, 1)
    for line in text.splitlines():
        m = _DAEMON_LINE.match(line.strip())
        if not m:
            continue
        ts, _level, rest = m.groups()
        fmt = "%Y-%m-%dT%H:%M:%S.%f" if "." in ts else "%Y-%m-%dT%H:%M:%S"
        dt = datetime.datetime.strptime(ts, fmt)
        guest_ns = (dt - epoch) // datetime.timedelta(microseconds=1) * 1000
        host_ns = guest_ns - offset_ns
        kv = dict(_KV.findall(rest))
        if rest.startswith("judge kill"):
            events["kills"].append({
                "t_ns": host_ns, "cand": kv.get("cand"), "dry_run": kv.get("dry_run") == "1",
                "result": int(kv["result"]) if "result" in kv else None, "path": kv.get("path"),
                "sys_avg_x100": kv.get("sys_avg_x100"), "io_avg_x100": kv.get("io_avg_x100"),
                "avail_x100": kv.get("avail_x100"),
            })
        elif rest.startswith("judge cand"):
            events["cands"].append({
                "t_ns": host_ns, "name": kv.get("name"), "protected": kv.get("protected") == "1",
                "eligible": kv.get("eligible") == "1",
            })
        elif rest.startswith("start "):
            events["start_mode"] = kv.get("mode")
    return events


def role_of(name):
    return ROLES.get(name or "", "other")


# ---------- 계산 ----------

def evaluate(d, now_ns=None, final=False):
    run = load_run(d)
    samples = load_samples(os.path.join(d, "samples.log"))
    ssh = sv.load_probes(os.path.join(d, "ssh_probe.tsv"))
    canary = sv.load_probes(os.path.join(d, "canary_probe.tsv"))
    buckets = build_buckets(samples)

    load_start = run.get("load_start_unix_ns")
    load_sec = run.get("load_sec")
    cap_ns = int(run.get("cap_sec") or DEFAULT_CAP_S) * NS
    observe_sec = run.get("observe_sec")
    offset_ns = int(run.get("guest_clock_offset_ns") or 0)

    last_seen = max([s["recv_ns"] for s in samples] + [t for t, _ in canary] + [0])
    if now_ns is None:
        now_ns = last_seen

    notes = []
    t0 = t_resolve = None
    status = "running"

    if load_start is None:
        notes.append("run.json에 load_start_unix_ns가 없어 T0를 계산하지 않았다")
    else:
        load_end = load_start + int(load_sec) * NS if load_sec else None
        start_idx = next((i for i, b in enumerate(buckets) if b["start_ns"] >= load_start), len(buckets))
        r0 = find_run(buckets, start_idx, lambda x: x >= FULL_RATIO, T0_RUN_S)
        if r0 is not None:
            t0 = buckets[r0[0]]["start_ns"]
            r1 = find_run(buckets, r0[0], lambda x: x < FULL_RATIO, RESOLVE_RUN_S)
            win_end_limit = t0 + cap_ns
            if load_end is not None:
                win_end_limit = min(win_end_limit, load_end)
            if r1 is not None and buckets[r1[1]]["end_ns"] <= win_end_limit:
                t_resolve = buckets[r1[1]]["end_ns"]
            if t_resolve is not None:
                status = "resolved"
            elif load_end is not None and now_ns >= load_end and load_end < t0 + cap_ns:
                status = "truncated"
            elif now_ns >= t0 + cap_ns:
                status = "unresolved"
        else:
            wait_s = observe_sec if observe_sec else DEFAULT_NO_THRASH_S
            if now_ns >= load_start + int(wait_s) * NS:
                status = "no_thrash"

    if final and status == "running":
        status = "incomplete"

    # 부하 시작 이후의 압력 최대치. 부하 크기를 맞출 때 "압력은 올랐는가, T0까지 갔는가"를 보는 값이다.
    peak_1s = peak_10s = None
    if load_start is not None:
        post = [b["ratio"] for b in buckets if b["start_ns"] >= load_start]
        valid = [r for r in post if r is not None]
        if valid:
            peak_1s = round(max(valid), 3)
        windows = [sum(post[i:i + 10]) / 10 for i in range(len(post) - 9) if None not in post[i:i + 10]]
        if windows:
            peak_10s = round(max(windows), 3)
    avail_min_mib = None
    if samples and load_start is not None:
        post_avail = [s["avail_kb"] for s in samples if s["recv_ns"] >= load_start]
        if post_avail:
            avail_min_mib = round(min(post_avail) / 1024, 1)

    # 커널 OOM
    t_oom = None
    oom_delta = 0
    if samples:
        base = samples[0]["oom_kill"]
        oom_delta = max(s["oom_kill"] for s in samples) - base
        t_oom = next((s["recv_ns"] for s in samples if s["oom_kill"] > base), None)

    # 데몬 판단
    dlog = parse_daemon_log(_read_text(os.path.join(d, "daemon.log")), offset_ns)
    first_decision = dlog["kills"][0] if dlog["kills"] else None
    real_kills = [k for k in dlog["kills"] if not k["dry_run"] and k["result"] == 0]
    t_detect = first_decision["t_ns"] if first_decision else None
    t_kill = real_kills[0]["t_ns"] if real_kills else None
    killed_roles = sorted({role_of(k["cand"]) for k in real_kills})

    # 필수 프로세스 E
    e_state = (_read_text(os.path.join(d, "e_state.txt")) or "").strip()
    e_alive = None
    if e_state:
        e_alive = "ActiveState=active" in e_state or e_state.split()[0] == "active"
    e_killed = ("E" in killed_roles) or (e_alive is False)

    # E 응답: T0부터 해소(또는 창 끝)까지. T0가 없으면 부하 시작부터 마지막 관찰까지.
    if t0 is not None:
        win_start, win_end = t0, (t_resolve if t_resolve is not None else min(t0 + cap_ns, now_ns))
    elif load_start is not None:
        win_start, win_end = load_start, now_ns
    else:
        win_start = win_end = 0
    e_canary = window_gap(canary, win_start, win_end)
    e_ssh = window_gap(ssh, win_start, win_end)

    def rel(t):
        return None if t is None or load_start is None else round((t - load_start) / NS, 1)

    def diff(a, b):
        return None if a is None or b is None else round((a - b) / NS, 1)

    # 선행 여부: 해소가 데몬의 종료 때문인지 커널 때문인지 (기록용)
    first_actor = "none"
    if t_kill is not None and (t_oom is None or t_kill < t_oom):
        first_actor = "daemon"
    elif t_oom is not None:
        first_actor = "kernel"
    daemon_before_kernel = None
    if t_detect is not None:
        daemon_before_kernel = t_oom is None or t_detect < t_oom

    return {
        "status": status,
        "t0_s": rel(t0),
        "t_detect_s": rel(t_detect),
        "t_kill_s": rel(t_kill),
        "t_resolve_s": rel(t_resolve),
        "t_oom_s": rel(t_oom),
        "resolve_time_s": diff(t_resolve, t0),
        "detect_delay_s": diff(t_detect, t0),
        "kill_to_resolve_s": diff(t_resolve, t_kill),
        "full_peak_1s": peak_1s,
        "full_peak_10s_avg": peak_10s,
        "mem_avail_min_mib": avail_min_mib,
        "kernel_oom_kills": oom_delta,
        "first_actor": first_actor,
        "daemon_decision_before_kernel": daemon_before_kernel,
        "decision_without_thrash": first_decision is not None and t0 is None,
        "decision_count": len(dlog["kills"]),
        "decisions": [
            {"t_s": rel(k["t_ns"]), "cand": k["cand"], "role": role_of(k["cand"]), "dry_run": k["dry_run"],
             "result": k["result"], "path": k["path"], "sys_avg_x100": k["sys_avg_x100"],
             "io_avg_x100": k["io_avg_x100"], "avail_x100": k["avail_x100"]}
            for k in dlog["kills"][:5]
        ],
        "protected_seen": sorted({c["name"] for c in dlog["cands"] if c["protected"] and c["name"]}),
        "killed_roles": killed_roles,
        "e_alive_at_end": e_alive,
        "e_killed": e_killed,
        "e_canary": e_canary,
        "e_ssh": e_ssh,
        "samples": len(samples),
        "buckets": len(buckets),
        "invalid_buckets": sum(1 for b in buckets if b["ratio"] is None),
        "daemon_log": bool(dlog["kills"] or dlog["start_mode"] or dlog["cands"]),
        "notes": notes,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dir", help="결과 디렉터리")
    ap.add_argument("--final", action="store_true", help="최종 계산 (running을 incomplete로 확정)")
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
