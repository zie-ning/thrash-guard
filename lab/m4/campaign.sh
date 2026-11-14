#!/bin/bash
# M4 본 실행 목록을 순서대로 돌린다 (설계: docs/validation/overview.md 4절).
#
# 폭주 2종(runaway_growth, spike_mem)을 dry-run과 켜짐(kill)으로 reps회씩, 정상 버스트(normal_burst)를
# dry-run으로 reps회 돌린다. 시간 흐름에 따른 치우침(VM 호스트 상태 등)이 한 조건에 몰리지 않도록 회차마다
# 모든 조건을 한 번씩 돌린다. 이미 result.json이 있는 실행은 건너뛰어 중단했다 다시 시작할 수 있다.
#
# 사용법: lab/m4/campaign.sh [--out-root DIR] [--reps N] [--list] [-- run.sh에 넘길 옵션...]
#   --out-root DIR   결과를 모을 디렉터리 (기본 results/m4/<날짜>)
#   --reps N         회차 수 (기본 3)
#   --loads "A B"    돌릴 부하만 고른다 (기본: runaway_growth spike_mem normal_burst)
#   --list           돌릴 목록만 보여 준다
#   --               이후의 옵션은 모든 run.sh 호출에 그대로 넘긴다 (예: -- --burst-mib 450)
#
# 모두 끝나면 결과 루트에 summary.md, summary.json과 부하마다 resolve_time_<부하>.svg를 만든다(lab/analyze/m4_summary.py).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_ROOT="$ROOT/results/m4/$(date +%Y%m%d)"
REPS=3
LOADS="runaway_growth spike_mem normal_burst"
LIST=0
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --out-root) OUT_ROOT="$2"; shift 2 ;;
        --reps) REPS="$2"; shift 2 ;;
        --loads) LOADS="$2"; shift 2 ;;
        --list) LIST=1; shift ;;
        --) shift; EXTRA=("$@"); break ;;
        *) echo "알 수 없는 옵션: $1" >&2; exit 2 ;;
    esac
done
[[ "$REPS" =~ ^[0-9]+$ ]] && [ "$REPS" -gt 0 ] || { echo "--reps는 1 이상의 정수여야 한다" >&2; exit 2; }

plan=()
for rep in $(seq 1 "$REPS"); do
    for load in $LOADS; do
        case "$load" in
            runaway_growth|spike_mem) plan+=("$load dry $rep" "$load kill $rep") ;;
            normal_burst) plan+=("normal_burst dry $rep") ;;
            *) echo "--loads에는 runaway_growth, spike_mem, normal_burst만 쓴다: $load" >&2; exit 2 ;;
        esac
    done
done

echo "실행 ${#plan[@]}회 -> $OUT_ROOT" >&2
failed=()
for item in "${plan[@]}"; do
    read -r load mode rep <<<"$item"
    id="$load-$mode-$rep"
    if [ -f "$OUT_ROOT/$id/result.json" ]; then
        echo "건너뜀(이미 있음): $id" >&2
        continue
    fi
    if [ "$LIST" = 1 ]; then
        echo "$id"
        continue
    fi
    echo "==== $id" >&2
    if ! "$ROOT/lab/m4/run.sh" --load "$load" --daemon "$mode" --id "$id" --out "$OUT_ROOT/$id" "${EXTRA[@]}"; then
        echo "실패: $id (다음 실행으로 넘어간다)" >&2
        failed+=("$id")
    fi
done

# 끝나면 요약표, 합불 판정, 그래프를 만든다 (lab/analyze/m4_summary.py). 실패해도 실행 결과는 그대로 남는다.
if [ "$LIST" != 1 ]; then
    python3 "$ROOT/lab/analyze/m4_summary.py" "$OUT_ROOT" || echo "요약을 만들지 못했다. 직접 실행: python3 lab/analyze/m4_summary.py $OUT_ROOT" >&2
fi

if [ "${#failed[@]}" -gt 0 ]; then
    echo "실패한 실행: ${failed[*]}" >&2
    exit 1
fi
