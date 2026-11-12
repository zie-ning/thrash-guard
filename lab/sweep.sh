#!/bin/bash
# 파라미터 탐색용: 부하 조건 목록을 차례로 lab/run.sh에 넘겨 실행한다 (M1-3).
#
# 사용법: lab/sweep.sh <탐색 이름> <조건>...
#   조건 형식: anon,file,rate[,pin]   (MiB, MiB, MiB/초, MiB)   예: 700,400,4 또는 700,0,4,43
#   pin은 mlock으로 고정해 둘 메모리 크기이며 생략하면 0이다. I/O 상한은 VM_IOPS 환경 변수로 바꾼다.
#   환경 변수: MAX_SEC(기본 1200), REPEAT(기본 1)
#
# 결과는 results/m1/<날짜>/<탐색 이름>/<번호>-a<anon>f<file>r<rate>-<회차>/ 에 쌓이고,
# 끝나면 lab/analyze/summary.py로 만든 요약(summary.md)을 같은 위치에 남긴다.
# 한 번의 실행이 실패해도 나머지를 계속 돌린다. 실패한 실행은 result.json이 없어 요약에서 빠진다.
set -uo pipefail
source "$(dirname "$0")/vm/config.sh"

name="${1:?탐색 이름이 필요하다}"
shift
[ $# -gt 0 ] || { echo "조건이 하나 이상 필요하다 (anon,file,rate[,pin])" >&2; exit 2; }
[[ "$name" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "탐색 이름에는 영문, 숫자, . _ - 만 쓴다" >&2; exit 2; }

MAX_SEC="${MAX_SEC:-1200}"
REPEAT="${REPEAT:-1}"
root="$TG_ROOT/results/m1/$(date +%Y%m%d)/$name"
mkdir -p "$root"

# 조건 형식을 미리 검사해, 몇 시간 뒤에야 오타를 발견하는 일을 막는다.
for spec in "$@"; do
    [[ "$spec" =~ ^[0-9]+,[0-9]+,[0-9]+(,[0-9]+)?$ ]] || { echo "잘못된 조건: $spec (anon,file,rate[,pin])" >&2; exit 2; }
done

n=0
for spec in "$@"; do
    pin=0
    IFS=, read -r anon file rate pin <<<"$spec"
    pin="${pin:-0}"
    for rep in $(seq 1 "$REPEAT"); do
        n=$((n + 1))
        id="a${anon}f${file}r${rate}"
        [ "$pin" -gt 0 ] && id="${id}p${pin}"
        out="$root/$(printf '%02d' "$n")-$id-$rep"
        log "[$n] anon=$anon file=$file rate=$rate pin=$pin IOPS=${VM_IOPS:-3000} 회차=$rep"
        "$TG_ROOT/lab/run.sh" --id "$id" --anon-mib "$anon" --file-mib "$file" --pin-mib "$pin" --rate-mib "$rate" \
            --max-sec "$MAX_SEC" --out "$out" >"$out.log" 2>&1 \
            || log "  실행 실패(종료 코드 $?), $out.log 확인"
        tail -n 2 "$out.log" | head -n 1 >&2
    done
done

python3 "$TG_ROOT/lab/analyze/summary.py" "$root" | tee "$root/summary.md"
