#!/bin/bash
# 실험 1회를 자동으로 실행한다 (M1-2).
#
#   VM 새로 켜기 → 메타데이터 → 수신기·SSH 프로브 → tg-sampler → tg-load → 판정 → VM 강제 종료 → 저장
#
# 판정은 lab/analyze/verdict.py가 한다(oom_kill / livelock / normal).
# 실행 중에도 5초마다 판정해, 결과가 확정되면 남은 시간을 기다리지 않고 끝낸다.
# 스래싱이 시작되면 SSH가 막혀 VM을 정리할 수 없으므로, 부하는 시작할 때 --duration을 정해 두고
# 종료는 항상 QMP 강제 종료로 한다.
#
# 사용법: lab/run.sh [옵션]
#   --id NAME         실행 이름(결과 디렉터리 이름에 붙는다, 기본 run)
#   --scenario S1|S2  시나리오. S1: swap 없음, 컨테이너 제한 없음. S2: swap + 컨테이너 메모리 상한
#   --cg-limit-mib N  S2 컨테이너 메모리 상한(기본 600). MemorySwapMax도 같은 값으로 둔다
#   --swap-mib N      S2 swap 크기(기본 2048)
#   --swappiness N    S2 vm.swappiness(기본 10)
#   --min-ttl-ms N    MGLRU min_ttl_ms를 이 값으로 바꾼다(생략하면 커널 기본값 그대로)
#   --anon-mib N      anon 목표 크기(기본 700)
#   --rate-mib N      anon 증가 속도, MiB/초(기본 4)
#   --file-mib N      file 모드 파일 크기(기본 400, 0이면 쓰지 않는다)
#   --pin-mib N       mlock으로 고정해 둘 메모리 크기(기본 0, 길벗의 회수 불가 메모리는 약 43)
#   --max-sec N       부하를 거는 최대 시간(기본 1500)
#   --baseline-sec N  부하 전 기준 구간(기본 15)
#   --out DIR         결과 디렉터리(기본 results/m1/<날짜>/<id>-<시각>)
#
# 위 기본값은 격자 탐색(M1-3) 전의 임시값이다. VM 사양은 환경 변수(VM_MEM_MB, VM_IOPS 등)로 바꾼다.
set -euo pipefail
source "$(dirname "$0")/vm/config.sh"

ID=run
SCENARIO=S1
ANON_MIB=700
RATE_MIB=4
FILE_MIB=400
PIN_MIB=0
CG_LIMIT_MIB=600
SWAP_MIB=2048
SWAPPINESS=10
MIN_TTL_MS=
MAX_SEC=1500
BASELINE_SEC=15
OUT=""
RECV_PORT=9999

while [ $# -gt 0 ]; do
    case "$1" in
        --id) ID="$2"; shift 2 ;;
        --scenario) SCENARIO="$2"; shift 2 ;;
        --anon-mib) ANON_MIB="$2"; shift 2 ;;
        --rate-mib) RATE_MIB="$2"; shift 2 ;;
        --file-mib) FILE_MIB="$2"; shift 2 ;;
        --pin-mib) PIN_MIB="$2"; shift 2 ;;
        --cg-limit-mib) CG_LIMIT_MIB="$2"; shift 2 ;;
        --swap-mib) SWAP_MIB="$2"; shift 2 ;;
        --swappiness) SWAPPINESS="$2"; shift 2 ;;
        --min-ttl-ms) MIN_TTL_MS="$2"; shift 2 ;;
        --max-sec) MAX_SEC="$2"; shift 2 ;;
        --baseline-sec) BASELINE_SEC="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "알 수 없는 옵션: $1" >&2; exit 2 ;;
    esac
done

[ "$SCENARIO" = "S1" ] || [ "$SCENARIO" = "S2" ] || { echo "S1 또는 S2만 지원한다 (S3는 M2에서 추가)" >&2; exit 2; }
if [ "$SCENARIO" = "S2" ] && { [ "$FILE_MIB" -gt 0 ] || [ "$PIN_MIB" -gt 0 ]; }; then
    echo "S2는 file·pin 부하를 쓰지 않는다 (--file-mib 0 --pin-mib 0)" >&2; exit 2
fi
[ -z "$MIN_TTL_MS" ] || [[ "$MIN_TTL_MS" =~ ^[0-9]+$ ]] || { echo "--min-ttl-ms는 0 이상의 정수여야 한다" >&2; exit 2; }
for v in ANON_MIB RATE_MIB FILE_MIB PIN_MIB CG_LIMIT_MIB SWAP_MIB SWAPPINESS MAX_SEC BASELINE_SEC; do
    [[ "${!v}" =~ ^[0-9]+$ ]] || { echo "$v 는 0 이상의 정수여야 한다: ${!v}" >&2; exit 2; }
done
[ "$ANON_MIB" -gt 0 ] || { echo "ANON_MIB는 1 이상이어야 한다" >&2; exit 2; }
[[ "$ID" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "--id에는 영문, 숫자, . _ - 만 쓴다" >&2; exit 2; }

OUT="${OUT:-$TG_ROOT/results/m1/$(date +%Y%m%d)/$ID-$(date +%H%M%S)}"
mkdir -p "$OUT"
log "결과 디렉터리: $OUT"

pids=()
cleanup() {
    for p in "${pids[@]}"; do kill -INT "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
}
trap 'cleanup; "$VM_DIR/stop.sh" --force >/dev/null 2>&1 || true' EXIT

make -C "$TG_ROOT" -s

log "VM 시작"
"$VM_DIR/reset-run.sh" >&2
"$TG_ROOT/lab/collect/run_meta.sh" "$OUT" >/dev/null

python3 "$TG_ROOT/lab/collect/tg_receiver.py" --port "$RECV_PORT" --out "$OUT/samples.log" \
    2>"$OUT/receiver.err" &
pids+=($!)
python3 "$TG_ROOT/lab/collect/ssh_probe.py" --port "$SSH_PORT" --out "$OUT/ssh_probe.tsv" \
    2>"$OUT/probe.err" &
pids+=($!)

vm_scp "$TG_ROOT/build/glibc/tg-sampler" "$TG_ROOT/build/glibc/tg-load" /tmp/
vm_ssh "sudo nohup /tmp/tg-sampler --dest $GUEST_HOST_IP:$RECV_PORT --interval-ms 250 \
        --cg-parent /sys/fs/cgroup/workload.slice >/tmp/sampler.out 2>&1 </dev/null &"
sleep 1
vm_ssh 'pgrep -x tg-sampler >/dev/null' \
    || { echo "tg-sampler가 시작되지 않았다" >&2; vm_ssh 'cat /tmp/sampler.out' >&2 || true; exit 1; }

# 커널 조절값. 읽어 온 값을 결과 디렉터리에 남겨, 실제로 적용됐는지 나중에 확인할 수 있게 한다.
if [ -n "$MIN_TTL_MS" ]; then
    log "MGLRU min_ttl_ms = $MIN_TTL_MS"
    vm_ssh "echo $MIN_TTL_MS | sudo tee /sys/kernel/mm/lru_gen/min_ttl_ms >/dev/null"
fi

if [ "$SCENARIO" = "S2" ]; then
    log "S2 환경: swap ${SWAP_MIB} MiB, swappiness ${SWAPPINESS}, 컨테이너 상한 ${CG_LIMIT_MIB} MiB"
    # swap 파일은 fallocate로 만든다. 쓰기 I/O가 I/O 상한에 걸려 오래 걸리는 일을 피한다.
    vm_ssh "sudo swapoff -a; sudo rm -f /swapfile; sudo fallocate -l ${SWAP_MIB}M /swapfile && sudo chmod 600 /swapfile \
            && sudo mkswap /swapfile >/dev/null && sudo swapon /swapfile && sudo sysctl -q vm.swappiness=$SWAPPINESS"
    # 컨테이너 대신 systemd 슬라이스로 상한을 건다. 이름이 workload-*.slice이면 workload.slice의 하위가 된다.
    vm_ssh "printf '[Slice]\nMemoryMax=${CG_LIMIT_MIB}M\nMemorySwapMax=${CG_LIMIT_MIB}M\n' \
            | sudo tee /run/systemd/system/workload-container.slice >/dev/null && sudo systemctl daemon-reload \
            && sudo systemctl start workload-container.slice"
    vm_ssh "sudo systemd-run -q --unit=tg-canary --slice=workload-container.slice \
            /tmp/tg-load canary --port $CANARY_PORT --work-kib 4096 --duration $((MAX_SEC + BASELINE_SEC + 120))"
    sleep 2
    vm_ssh 'systemctl is-active tg-canary >/dev/null' \
        || { echo "카나리가 시작되지 않았다" >&2; vm_ssh 'sudo journalctl -u tg-canary --no-pager | tail' >&2 || true; exit 1; }
    python3 "$TG_ROOT/lab/collect/canary_probe.py" --port "$CANARY_PORT" --out "$OUT/canary_probe.tsv" \
        2>"$OUT/canary_probe.err" &
    pids+=($!)
fi
vm_ssh 'echo "min_ttl_ms=$(cat /sys/kernel/mm/lru_gen/min_ttl_ms 2>/dev/null) lru_gen_enabled=$(cat /sys/kernel/mm/lru_gen/enabled 2>/dev/null) swappiness=$(cat /proc/sys/vm/swappiness) thp=$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null)"; swapon --show --noheadings; cat /sys/fs/cgroup/workload.slice/workload-container.slice/memory.max /sys/fs/cgroup/workload.slice/workload-container.slice/memory.swap.max 2>/dev/null' \
    >"$OUT/kernel_knobs.txt" 2>&1 || true

# 파일은 부하를 걸기 전에 만들어 둔다. 만드는 시간이 측정에 섞이지 않게 하기 위함이다.
if [ "$FILE_MIB" -gt 0 ]; then
    log "file 모드 파일 준비 (${FILE_MIB} MiB)"
    vm_ssh "sudo /tmp/tg-load file --path /var/tmp/tg.dat --size-mib $FILE_MIB --prepare-only"
fi

# 회수 불가 메모리는 기준 구간 전에 고정해, 부하가 아니라 "원래 있던 메모리"로 보이게 한다.
if [ "$PIN_MIB" -gt 0 ]; then
    log "mlock 메모리 ${PIN_MIB} MiB 고정"
    vm_ssh "sudo nohup /tmp/tg-load pin --size-mib $PIN_MIB --duration $((MAX_SEC + BASELINE_SEC + 120)) \
            >/tmp/tg-pin.out 2>&1 </dev/null &"
    sleep 2
    vm_ssh 'pgrep -f "^/tmp/tg-load pin" >/dev/null' \
        || { echo "pin 모드가 시작되지 않았다" >&2; vm_ssh 'cat /tmp/tg-pin.out' >&2 || true; exit 1; }
fi

log "기준 구간 ${BASELINE_SEC}초"
sleep "$BASELINE_SEC"

LOAD_START_NS="$(date +%s%N)"
log "부하 시작: anon ${ANON_MIB} MiB (${RATE_MIB} MiB/s), file ${FILE_MIB} MiB, pin ${PIN_MIB} MiB, 최대 ${MAX_SEC}초"
if [ "$SCENARIO" = "S2" ]; then
    vm_ssh "sudo systemd-run -q --unit=tg-anon --slice=workload-container.slice \
            -p StandardOutput=file:/tmp/tg-anon.out -p StandardError=file:/tmp/tg-anon.out \
            /tmp/tg-load anon --size-mib $ANON_MIB --rate-mib $RATE_MIB --duration $MAX_SEC"
else
    vm_ssh "sudo nohup /tmp/tg-load anon --size-mib $ANON_MIB --rate-mib $RATE_MIB --duration $MAX_SEC \
            >/tmp/tg-anon.out 2>&1 </dev/null &"
fi
if [ "$FILE_MIB" -gt 0 ]; then
    vm_ssh "sudo nohup /tmp/tg-load file --path /var/tmp/tg.dat --size-mib $FILE_MIB --duration $MAX_SEC \
            >/tmp/tg-file.out 2>&1 </dev/null &"
fi
sleep 2
# 부하가 실제로 걸리지 않았는데 결과가 "정상"으로 기록되는 일을 막는다.
vm_ssh 'pgrep -x tg-load >/dev/null' \
    || { echo "tg-load가 시작되지 않았다" >&2; vm_ssh 'cat /tmp/tg-anon.out' >&2 || true; exit 1; }

# 실행 조건 기록. 판정 결과는 끝난 뒤 result.json에 합친다.
RUN_ID="$ID" SCENARIO="$SCENARIO" ANON_MIB="$ANON_MIB" RATE_MIB="$RATE_MIB" FILE_MIB="$FILE_MIB" \
PIN_MIB="$PIN_MIB" IO_IOPS="$VM_IOPS" IO_BPS="$VM_BPS" \
CG_LIMIT_MIB="$CG_LIMIT_MIB" SWAP_MIB="$SWAP_MIB" SWAPPINESS="$SWAPPINESS" MIN_TTL_MS="$MIN_TTL_MS" \
MAX_SEC="$MAX_SEC" BASELINE_SEC="$BASELINE_SEC" LOAD_START_NS="$LOAD_START_NS" \
python3 - "$OUT/run.json" <<'EOF'
import json, os, sys
e = os.environ
json.dump({
    "id": e["RUN_ID"], "scenario": e["SCENARIO"],
    "load": {"anon_mib": int(e["ANON_MIB"]), "rate_mib_s": int(e["RATE_MIB"]),
             "file_mib": int(e["FILE_MIB"]), "pin_mib": int(e["PIN_MIB"]), "pattern": "random"},
    "io_limit": {"iops": int(e["IO_IOPS"]), "bytes_per_sec": int(e["IO_BPS"])},
    "kernel": {"min_ttl_ms": int(e["MIN_TTL_MS"]) if e["MIN_TTL_MS"] else None},
    "container": ({"limit_mib": int(e["CG_LIMIT_MIB"]), "swap_mib": int(e["SWAP_MIB"]),
                   "swappiness": int(e["SWAPPINESS"])} if e["SCENARIO"] == "S2" else None),
    "max_sec": int(e["MAX_SEC"]), "baseline_sec": int(e["BASELINE_SEC"]),
    "load_start_unix_ns": int(e["LOAD_START_NS"]),
}, open(sys.argv[1], "w"), indent=2)
EOF

# 5초마다 판정한다. 확정되면 바로 끝낸다.
deadline=$((SECONDS + MAX_SEC + 30))
status=running
while [ "$SECONDS" -lt "$deadline" ]; do
    sleep 5
    if [ "$SCENARIO" = "S2" ]; then
        status="$(python3 "$TG_ROOT/lab/analyze/s2_summary.py" "$OUT" --now-ns "$(date +%s%N)" --field verdict)"
    else
        status="$(python3 "$TG_ROOT/lab/analyze/verdict.py" "$OUT" --now-ns "$(date +%s%N)" --field verdict)"
    fi
    [ "$status" = "running" ] || break
done

END_NS="$(date +%s%N)"
if [ "$SCENARIO" = "S2" ]; then
    JUDGE="$TG_ROOT/lab/analyze/s2_summary.py"
else
    JUDGE="$TG_ROOT/lab/analyze/verdict.py"
fi
python3 "$JUDGE" "$OUT" --now-ns "$END_NS" --final >"$OUT/verdict.json"
VERDICT="$(python3 "$JUDGE" "$OUT" --now-ns "$END_NS" --final --field verdict)"

# VM이 아직 응답하면(= livelock이 아니면) 부하 로그와 커널 OOM 기록을 가져온다.
if [ "$VERDICT" != "livelock" ]; then
    timeout 30 ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" \
        'tail -n 30 /tmp/tg-anon.out /tmp/tg-file.out /tmp/tg-pin.out 2>/dev/null; sudo dmesg | grep -i -E "oom|killed process" || true' \
        >"$OUT/load.out" 2>&1 || echo "VM이 응답하지 않아 부하 로그를 가져오지 못했다" >"$OUT/load.out"
fi

cleanup
pids=()
python3 "$TG_ROOT/lab/collect/tg_to_csv.py" "$OUT/samples.log" --out "$OUT/samples.csv" || true
python3 "$TG_ROOT/lab/collect/tg_to_csv.py" "$OUT/samples.log" --kind cg --out "$OUT/cg.csv" 2>/dev/null || true

"$VM_DIR/stop.sh" --force >/dev/null 2>&1 || true

python3 - "$OUT" <<'EOF'
import json, sys
d = sys.argv[1]
run = json.load(open(f"{d}/run.json"))
res = json.load(open(f"{d}/verdict.json"))
json.dump({**run, "result": res}, open(f"{d}/result.json", "w"), ensure_ascii=False, indent=2)
extra = f", 카나리 최장 무응답 {res['canary_max_gap_s']}초" if "canary_max_gap_s" in res else ""
print(f"판정: {res['verdict']}  (SSH 최장 무응답 {res['ssh_max_gap_s']}초{extra}, "
      f"MemAvailable 최소 {res['mem_avail_min_mib']} MiB, OOM kill {res['oom_kill_delta']}건, "
      f"샘플 {res['samples']}개)")
EOF
log "완료: $OUT"
