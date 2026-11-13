#!/usr/bin/env python3
"""M4 본 실행 결과를 모아 요약표, 합불 판정, 그래프를 만든다 (설계: docs/validation/overview.md 5절).

입력: 결과 루트 디렉터리(예: results/m4/20261114) 바로 아래의 <실행>/result.json.
      lab/m4/run.sh가 실행마다 만들고, 보정용인 results/m4/smoke, results/m4/prelim은 루트가 달라 섞이지 않는다.
출력(--out, 기본은 루트): summary.md(표와 판정), summary.json(원자료),
      resolve_time_<부하>.svg(부하마다 하나. 가로축은 회차, 회차마다 켜짐과 dry-run 막대를 나란히 놓는다)

합불 판정(설계 문서 5절, 임시 기준):
  해소 시간  부하마다 켜짐(kill) 중앙값이 dry-run 중앙값보다 짧고 60초 이내. 5분 안에 풀리지 않은 실행(unresolved)은
             무한대로 센다. resolved와 unresolved 실행이 부하·조건마다 2개 미만이면 판정 불가
  E 생존     켜짐 실행에서 보호된 E가 한 번도 종료되지 않았고 종료 시점에 active
  정상 버스트 정상 버스트 실행의 판단 로그에 종료 결정이 한 번도 없음
  안전 항목(E 생존, 정상 버스트)은 한 번이라도 어기면 불합격이다. 로그를 못 가져와 확인할 수 없으면 판정 불가다.
기록 지표(합불에 쓰지 않음): E 응답(카나리, SSH), 감지 지연, 종료 후 회복 지연, 데몬 선행, 커널 OOM 등.
"""

import argparse
import json
import math
import os
import statistics
import sys
from xml.sax.saxutils import escape

STRESS_LOADS = ["runaway_growth", "spike_mem"]       # 그래프의 가로축 (폭주)
LOAD_LABEL = {"runaway_growth": "RUNAWAY_GROWTH", "spike_mem": "SPIKE_MEM", "normal_burst": "정상 버스트"}
RESOLVE_LIMIT_S = 60       # 해소 시간 합격 상한 (임시)
MIN_VALID = 2              # 해소 시간 비교에 필요한 유효 실행 수 (부하·조건마다)
EXPECTED_REPS = 3          # 설계의 반복 횟수
DEFAULT_CAP_S = 300


# ---------- 읽기와 묶기 ----------

def load_runs(root):
    """root 바로 아래 하위 디렉터리의 result.json을 읽는다. 읽을 수 없거나 형식이 다르면 건너뛴다."""
    runs = []
    for name in sorted(os.listdir(root)):
        path = os.path.join(root, name, "result.json")
        if not os.path.isfile(path):
            continue
        try:
            with open(path, encoding="utf-8") as f:
                r = json.load(f)
        except (OSError, ValueError):
            continue
        if isinstance(r, dict) and "result" in r and "load" in r and "daemon" in r:
            r["_dir"] = name
            runs.append(r)
    return runs


def group_runs(runs):
    groups = {}
    for r in runs:
        groups.setdefault((r["load"], r["daemon"]), []).append(r)
    return groups


# ---------- 값 계산 ----------

def median(values):
    return statistics.median(values) if values else None


def resolve_value(r):
    """해소 시간 비교에 쓰는 값. resolved는 초, unresolved는 무한대, 그 밖(truncated, no_thrash, incomplete)은 None."""
    res = r["result"]
    if res["status"] == "resolved" and res.get("resolve_time_s") is not None:
        return res["resolve_time_s"]
    if res["status"] == "unresolved":
        return math.inf
    return None


def fmt(v, unit=""):
    if v is None:
        return "-"
    if v == math.inf:
        return "미해소"
    return f"{v:.1f}{unit}"


def fmt_range(values):
    if not values:
        return "-"
    return f"{fmt(min(values))}~{fmt(max(values))}"


def count_by(values):
    out = {}
    for v in values:
        out[v] = out.get(v, 0) + 1
    return out


def group_stats(rs):
    res = [r["result"] for r in rs]
    resolve = [v for v in (resolve_value(r) for r in rs) if v is not None]

    def finite(key):
        return [x[key] for x in res if x.get(key) is not None]

    canary = [x["e_canary"]["max_gap_s"] for x in res if x.get("e_canary", {}).get("max_gap_s") is not None]
    canary_ok = [x["e_canary"]["ok_rate"] for x in res if x.get("e_canary", {}).get("ok_rate") is not None]
    ssh = [x["e_ssh"]["max_gap_s"] for x in res if x.get("e_ssh", {}).get("max_gap_s") is not None]
    return {
        "n": len(rs),
        "ids": [r.get("id", r["_dir"]) for r in rs],
        "status": count_by([x["status"] for x in res]),
        "resolve_values": resolve,
        "resolve_median": median(resolve),
        "t0_median": median(finite("t0_s")),
        "t0_seen": sum(1 for x in res if x.get("t0_s") is not None),
        "detect_delay_median": median(finite("detect_delay_s")),
        "kill_to_resolve_median": median(finite("kill_to_resolve_s")),
        "canary_gap_values": canary,
        "canary_gap_median": median(canary),
        "canary_ok_median": median(canary_ok),
        "ssh_gap_values": ssh,
        "ssh_gap_median": median(ssh),
        "first_actor": count_by([x.get("first_actor", "none") for x in res]),
        "kernel_oom_runs": sum(1 for x in res if x.get("kernel_oom_kills", 0) > 0),
        "decision_count": sum(x.get("decision_count", 0) for x in res),
        "killed_roles": sorted({role for x in res for role in x.get("killed_roles", [])}),
    }


# ---------- 합불 판정 ----------

def judge_resolve(groups, load):
    dry = [v for v in (resolve_value(r) for r in groups.get((load, "dry"), [])) if v is not None]
    kill = [v for v in (resolve_value(r) for r in groups.get((load, "kill"), [])) if v is not None]
    out = {"load": load, "dry_values": dry, "kill_values": kill, "limit_s": RESOLVE_LIMIT_S}
    if len(dry) < MIN_VALID or len(kill) < MIN_VALID:
        out.update(verdict="판정 불가",
                   reason=f"유효 실행이 부족하다(dry-run {len(dry)}개, 켜짐 {len(kill)}개, 각 {MIN_VALID}개 이상 필요). "
                          f"스래싱이 나지 않았거나 실행이 확정되지 않은 것이다")
        return out
    dm, km = median(dry), median(kill)
    out.update(dry_median=dm, kill_median=km, kill_worst=max(kill))
    faster = km < dm
    within = km <= RESOLVE_LIMIT_S
    out["verdict"] = "합격" if (faster and within) else "불합격"
    parts = [f"켜짐 중앙값 {fmt(km)}초 / dry-run 중앙값 {fmt(dm)}{'' if dm == math.inf else '초'}",
             f"켜짐 최악 {fmt(max(kill))}{'' if max(kill) == math.inf else '초'}"]
    if not faster:
        parts.append("켜짐이 dry-run보다 짧지 않다")
    if not within:
        parts.append(f"켜짐이 {RESOLVE_LIMIT_S}초를 넘는다")
    out["reason"] = ", ".join(parts)
    return out


def judge_e_survival(groups):
    kill = [r for (load, daemon), rs in groups.items() if daemon == "kill" for r in rs]
    out = {"runs": len(kill)}
    if not kill:
        out.update(verdict="판정 불가", reason="켜짐 실행이 없다")
        return out
    killed = [r.get("id", r["_dir"]) for r in kill if r["result"].get("e_killed")]
    unknown = [r.get("id", r["_dir"]) for r in kill
               if r["result"].get("e_alive_at_end") is None and not r["result"].get("e_killed")]
    if killed:
        out.update(verdict="불합격", reason=f"E가 종료된 실행: {', '.join(killed)}")
    elif unknown:
        out.update(verdict="판정 불가", reason=f"종료 시점의 E 상태를 가져오지 못한 실행: {', '.join(unknown)}")
    else:
        out.update(verdict="합격", reason=f"켜짐 {len(kill)}회 모두 E가 종료되지 않고 active")
    return out


def judge_normal_burst(groups):
    nb = [r for (load, _), rs in groups.items() if load == "normal_burst" for r in rs]
    out = {"runs": len(nb)}
    if not nb:
        out.update(verdict="판정 불가", reason="정상 버스트 실행이 없다")
        return out
    decisions = sum(r["result"].get("decision_count", 0) for r in nb)
    no_log = [r.get("id", r["_dir"]) for r in nb if not r["result"].get("daemon_log")]
    if decisions:
        out.update(verdict="불합격", reason=f"종료 결정(오탐) {decisions}건")
    elif no_log:
        out.update(verdict="판정 불가", reason=f"데몬 로그를 가져오지 못해 결정 여부를 확인할 수 없다: {', '.join(no_log)}")
    else:
        out.update(verdict="합격", reason=f"{len(nb)}회 모두 종료 결정 0건")
    return out


# ---------- 주의할 실행 ----------

def warnings_for(runs):
    out = []
    for r in runs:
        res = r["result"]
        rid = r.get("id", r["_dir"])
        if res["status"] in ("incomplete", "truncated"):
            out.append((rid, f"상태 {res['status']}: 해소 시간을 주장할 수 없다"))
        if res["status"] == "no_thrash" and r["load"] in STRESS_LOADS:
            out.append((rid, "폭주 부하인데 스래싱이 나지 않았다(no_thrash)"))
        if res.get("psi", {}).get("disagree"):
            out.append((rid, "SSH 기준과 PSI 보조 기록의 T0가 어긋난다"))
        if res.get("invalid_buckets", 0) > 0:
            out.append((rid, f"샘플 누락으로 무효인 1초 구간이 {res['invalid_buckets']}개 있다"))
        if not res.get("daemon_log") and r["daemon"] != "none":
            out.append((rid, "데몬 로그가 없다"))
        if res.get("e_alive_at_end") is None and r["daemon"] != "none":
            out.append((rid, "종료 시점의 E 상태가 없다"))
    return out


# ---------- 그래프 ----------
# 부하마다 그래프 하나. 가로축은 회차, 회차마다 Thrashing Guard(초록)와 Linux Kernel(파랑) 막대를 나란히 놓는다.
# 모양은 Facebook oomd 글의 그림을 따른다(연한 회색 바탕, 흰 그래프 영역, 왼쪽 위 범례, 눈금 바깥쪽).
# 세로축은 0부터 측정 상한(기본 300초)까지 50초 단위이고, 상한 위로 조금 여유를 둔다.
# 5분 안에 풀리지 않은 실행(미해소)은 상한 높이(300초)까지 찬 막대로 그리고, 해소 시간을 낼 수 없는 실행은 막대를 비운다.

COLORS = {"kill": "#a6d075", "dry": "#39aed6"}       # 데몬 켜짐(처치), dry-run(대조). 참고 그림의 oomd, kernel 색
LABELS = {"kill": "Thrashing Guard", "dry": "Linux Kernel"}
LEGEND_TEXT_X = 38         # 범례 박스 왼쪽에서 글자 시작까지(견본 사각형 뒤)
LEGEND_CHAR_PX = 8.5       # 범례 글자 폭 어림(DejaVu Sans 13px는 평균 7~8px)
LEGEND_PAD_PX = 12         # 글자 뒤 오른쪽 여백
Y_STEP_S = 50              # 세로축 눈금 간격(초)
Y_HEADROOM = 1.05          # 상한 위의 여유 비율
FONT = "DejaVu Sans, Malgun Gothic, Noto Sans CJK KR, sans-serif"


def chart_name(load):
    return f"resolve_time_{load}.svg"


def chart_loads(groups):
    """그래프를 그릴 부하: 폭주 부하 가운데 실행이 있는 것."""
    return [load for load in STRESS_LOADS if (load, "dry") in groups or (load, "kill") in groups]


def rep_of(run, fallback):
    """실행 id 끝의 회차 번호(예: spike_mem-dry-2 -> 2). 없으면 정렬 순서."""
    tail = str(run.get("id", run["_dir"])).rsplit("-", 1)[-1]
    return int(tail) if tail.isdigit() else fallback


def rep_values(runs):
    """{회차: 해소 시간(초), 미해소는 무한대, 유효하지 않은 실행은 None}"""
    out = {}
    for i, r in enumerate(sorted(runs, key=lambda r: str(r.get("id", r["_dir"]))), 1):
        out[rep_of(r, i)] = resolve_value(r)
    return out


def axis_for(cap_s):
    """세로축: (눈금 간격, 마지막 눈금, 축 끝값). 마지막 눈금은 측정 상한 이상의 50초 배수다."""
    last = int(math.ceil(cap_s / Y_STEP_S) * Y_STEP_S)
    return Y_STEP_S, last, last * Y_HEADROOM


def render_load_svg(groups, load, cap_s):
    width, height = 640, 480
    ax_l, ax_t, ax_r, ax_b = 84, 58, width - 26, height - 72
    pw, ph = ax_r - ax_l, ax_b - ax_t
    dry = rep_values(groups.get((load, "dry"), []))
    kill = rep_values(groups.get((load, "kill"), []))
    reps = sorted(set(dry) | set(kill)) or [1]
    n = len(reps)
    step, last_tick, top = axis_for(cap_s)
    xmin, xmax = 0.4, n + 0.6

    def px(x):
        return ax_l + (x - xmin) / (xmax - xmin) * pw

    def py(v):
        return ax_b - min(v, top) / top * ph

    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" '
         f'font-family="{FONT}" font-size="13" fill="#262626">',
         f'<rect width="{width}" height="{height}" fill="#f0f1f3"/>',
         f'<rect x="{ax_l}" y="{ax_t}" width="{pw}" height="{ph}" fill="white" stroke="black" stroke-width="1"/>',
         f'<text x="{ax_l + pw / 2:.1f}" y="{ax_t - 14}" text-anchor="middle" font-size="16">{escape(LOAD_LABEL[load])}</text>']
    # y 눈금: 0부터 마지막 눈금까지 50초 단위
    for v in range(0, last_tick + 1, step):
        yy = py(v)
        s.append(f'<line x1="{ax_l - 5}" y1="{yy:.1f}" x2="{ax_l}" y2="{yy:.1f}" stroke="black"/>'
                 f'<text x="{ax_l - 9}" y="{yy + 4.5:.1f}" text-anchor="end">{v}</text>')
    # 막대: 회차마다 Thrashing Guard(왼쪽), Linux Kernel(오른쪽). 값이 없거나 무효인 자리는 비워 둔다.
    bw = 0.35
    for i, rep in enumerate(reps):
        c = i + 1
        for kind, data, x_left in (("kill", kill, c - bw), ("dry", dry, c)):
            val = data.get(rep)
            if val is not None:
                shown = cap_s if val == math.inf else val
                x0, x1 = px(x_left), px(x_left + bw)
                s.append(f'<rect class="bar" x="{x0:.1f}" y="{py(shown):.1f}" width="{x1 - x0:.1f}" '
                         f'height="{ax_b - py(shown):.1f}" fill="{COLORS[kind]}"/>')
        s.append(f'<line x1="{px(c):.1f}" y1="{ax_b}" x2="{px(c):.1f}" y2="{ax_b + 5}" stroke="black"/>'
                 f'<text x="{px(c):.1f}" y="{ax_b + 20}" text-anchor="middle">{rep}</text>')
    # 축 이름
    s.append(f'<text x="{ax_l + pw / 2:.1f}" y="{height - 22}" text-anchor="middle">회차</text>')
    s.append(f'<text x="22" y="{ax_t + ph / 2:.1f}" transform="rotate(-90 22 {ax_t + ph / 2:.1f})" text-anchor="middle">'
             f'스래싱 해소 시간 (초)</text>')
    # 범례(왼쪽 위, 그래프 안)
    lx, ly = ax_l + 10, ax_t + 10
    # 박스 너비는 가장 긴 문구에 맞춘다. 글꼴마다 폭이 달라 한 글자를 넉넉히 8.5px로 어림하고 오른쪽 여백을 둔다.
    legend_w = LEGEND_TEXT_X + int(math.ceil(max(len(t) for t in LABELS.values()) * LEGEND_CHAR_PX)) + LEGEND_PAD_PX
    s.append(f'<rect x="{lx}" y="{ly}" width="{legend_w}" height="52" rx="3" fill="white" fill-opacity="0.8" stroke="#cccccc"/>')
    for k, kind in enumerate(("kill", "dry")):
        yy = ly + 8 + k * 21
        s.append(f'<rect x="{lx + 9}" y="{yy}" width="22" height="12" fill="{COLORS[kind]}"/>'
                 f'<text x="{lx + LEGEND_TEXT_X}" y="{yy + 10.5}">{escape(LABELS[kind])}</text>')
    s.append('</svg>')
    return "\n".join(s) + "\n"


# ---------- 보고서 ----------

def rows_table(groups):
    lines = ["| 부하 | 조건 | 실행 | 상태 | 해소 시간 중앙값(초) | 범위(초) | T0 중앙값(초) | 감지 지연(초) | 종료→해소(초) |",
             "|---|---|---|---|---|---|---|---|---|"]
    for load in STRESS_LOADS + ["normal_burst"]:
        for daemon in ("dry", "kill", "none"):
            rs = groups.get((load, daemon))
            if not rs:
                continue
            st = group_stats(rs)
            status = ", ".join(f"{k} {v}" for k, v in sorted(st["status"].items()))
            lines.append(f"| {LOAD_LABEL[load]} | {daemon} | {st['n']} | {status} | {fmt(st['resolve_median'])} | "
                         f"{fmt_range(st['resolve_values'])} | {fmt(st['t0_median'])} | {fmt(st['detect_delay_median'])} | "
                         f"{fmt(st['kill_to_resolve_median'])} |")
    return lines


def record_table(groups):
    lines = ["| 부하 | 조건 | E 카나리 최대 무응답 중앙값(초) | 범위(초) | 카나리 성공률 중앙값 | SSH 최대 무응답 중앙값(초) | 데몬 선행 | 커널 OOM 실행 | 종료 결정 수 |",
             "|---|---|---|---|---|---|---|---|---|"]
    for load in STRESS_LOADS + ["normal_burst"]:
        for daemon in ("dry", "kill", "none"):
            rs = groups.get((load, daemon))
            if not rs:
                continue
            st = group_stats(rs)
            actor = ", ".join(f"{k} {v}" for k, v in sorted(st["first_actor"].items()))
            ok = st["canary_ok_median"]
            lines.append(f"| {LOAD_LABEL[load]} | {daemon} | {fmt(st['canary_gap_median'])} | {fmt_range(st['canary_gap_values'])} | "
                         f"{'-' if ok is None else f'{ok * 100:.0f}%'} | {fmt(st['ssh_gap_median'])} | {actor} | "
                         f"{st['kernel_oom_runs']} | {st['decision_count']} |")
    return lines


def build_summary(runs):
    groups = group_runs(runs)
    caps = [r.get("cap_sec") for r in runs if r.get("cap_sec")]
    cap_s = max(caps) if caps else DEFAULT_CAP_S
    judgments = {
        "resolve": {load: judge_resolve(groups, load) for load in STRESS_LOADS
                    if (load, "dry") in groups or (load, "kill") in groups},
        "e_survival": judge_e_survival(groups),
        "normal_burst": judge_normal_burst(groups),
    }
    return groups, cap_s, judgments


def render_markdown(root, runs, groups, cap_s, judgments):
    out = ["# M4 효과 검증 요약", "",
           f"- 입력: `{root}` (실행 {len(runs)}개). 결과 루트 하나의 실행만 모은다. 보정용 실행은 `results/m4/smoke`, `results/m4/prelim`에 따로 있고 본 실행의 평가에는 쓰지 않는다.",
           f"- 해소 시간은 T0(SSH 프로브 3번 연속 실패)부터 해소(5번 연속 성공)까지이고, T0 이후 {cap_s}초 안에 풀리지 않으면 미해소다.",
           "- 판정 기준은 임시값이며([docs/validation/overview.md](../../../docs/validation/overview.md) 5절), 각 부하 3회씩의 관찰이다.", ""]
    for key, label in (("dry", "dry-run"), ("kill", "켜짐")):
        for load in STRESS_LOADS:
            n = len(groups.get((load, key), []))
            if 0 < n < EXPECTED_REPS:
                out.append(f"- 주의: {LOAD_LABEL[load]} {label} 실행은 {n}회뿐이다(설계는 {EXPECTED_REPS}회).")
    nb = len(groups.get(("normal_burst", "dry"), []))
    if 0 < nb < EXPECTED_REPS:
        out.append(f"- 주의: 정상 버스트 실행은 {nb}회뿐이다(설계는 {EXPECTED_REPS}회).")
    out += ["", "## 합불 판정", "", "| 항목 | 주장 | 결과 | 근거 |", "|---|---|---|---|"]
    for load, j in judgments["resolve"].items():
        out.append(f"| 해소 시간 ({LOAD_LABEL[load]}) | V2 | **{j['verdict']}** | {j['reason']} |")
    j = judgments["e_survival"]
    out.append(f"| E 생존 | V3 | **{j['verdict']}** | {j['reason']} |")
    j = judgments["normal_burst"]
    out.append(f"| 정상 버스트 | V4 | **{j['verdict']}** | {j['reason']} |")
    out += ["", "## 해소 시간 그래프", ""]
    for load in chart_loads(groups):
        out += [f"![{LOAD_LABEL[load]}]({chart_name(load)})", ""]
    out += [f"회차마다 왼쪽(초록)이 Thrashing Guard(데몬 켜짐), 오른쪽(파랑)이 Linux Kernel(dry-run, 데몬 꺼짐 대용)이다. "
            f"{cap_s}초까지 찬 막대는 T0 이후 5분 안에 풀리지 않은 실행(미해소)이다. 막대가 없는 자리는 해소 시간을 낼 수 없는 실행"
            f"(스래싱 미발생, 창 잘림 등)이거나 실행이 없는 경우이며, 아래 실행 목록에서 확인한다.", "", "## 해소 시간", ""] + rows_table(groups)
    out += ["", "## 기록 지표 (합불에 쓰지 않음)", ""] + record_table(groups)
    warns = warnings_for(runs)
    out += ["", "## 주의할 실행", ""]
    out += [f"- `{rid}`: {msg}" for rid, msg in warns] if warns else ["없음"]
    out += ["", "## 실행 목록", "", "| 실행 | 부하 | 조건 | 상태 | T0(초) | 해소 시간(초) | 데몬 결정(초) |", "|---|---|---|---|---|---|---|"]
    for r in runs:
        x = r["result"]
        out.append(f"| {r.get('id', r['_dir'])} | {r['load']} | {r['daemon']} | {x['status']} | {fmt(x.get('t0_s'))} | "
                   f"{fmt(x.get('resolve_time_s'))} | {fmt(x.get('t_detect_s'))} |")
    return "\n".join(out) + "\n"


def sanitize(o):
    """JSON 표준에 없는 무한대(미해소)를 문자열 "inf"로 바꾼다. 엄격한 파서도 읽을 수 있게 한다."""
    if isinstance(o, float) and math.isinf(o):
        return "inf"
    if isinstance(o, dict):
        return {k: sanitize(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [sanitize(v) for v in o]
    return o


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("root", help="결과 루트 디렉터리 (예: results/m4/20261114)")
    ap.add_argument("--out", help="출력 디렉터리 (기본: root)")
    args = ap.parse_args()

    runs = load_runs(args.root)
    if not runs:
        print(f"{args.root}에서 result.json을 찾지 못했다", file=sys.stderr)
        return 1
    groups, cap_s, judgments = build_summary(runs)
    out = args.out or args.root
    os.makedirs(out, exist_ok=True)

    with open(os.path.join(out, "summary.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(render_markdown(args.root, runs, groups, cap_s, judgments))
    charts = chart_loads(groups)
    for load in charts:
        with open(os.path.join(out, chart_name(load)), "w", encoding="utf-8", newline="\n") as f:
            f.write(render_load_svg(groups, load, cap_s))
    summary = {
        "root": args.root, "runs": len(runs), "cap_s": cap_s, "judgments": judgments,
        "groups": {f"{load}-{daemon}": group_stats(rs) for (load, daemon), rs in sorted(groups.items())},
        "warnings": [{"run": rid, "message": msg} for rid, msg in warnings_for(runs)],
    }
    with open(os.path.join(out, "summary.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(sanitize(summary), f, ensure_ascii=False, indent=2, allow_nan=False)
        f.write("\n")

    for load, j in judgments["resolve"].items():
        print(f"해소 시간 ({LOAD_LABEL[load]}): {j['verdict']} - {j['reason']}")
    print(f"E 생존: {judgments['e_survival']['verdict']} - {judgments['e_survival']['reason']}")
    print(f"정상 버스트: {judgments['normal_burst']['verdict']} - {judgments['normal_burst']['reason']}")
    print(f"저장: {out}/summary.md, summary.json, " + ", ".join(chart_name(load) for load in charts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
