#!/bin/bash
# M4 효과 검증 실행 1회 (설계: docs/validation/overview.md, 정의: docs/validation/measurement.md).
#
#   VM 새로 켜기 → 수집기 → (데몬) → 필수 프로세스 E → 기준 구간 → 부하 → 끝날 때까지 감시
#   → (VM이 응답하면) 로그 수집 → VM 종료 → 결과 계산(result.json)
#
# 부하와 E는 각각 별도 cgroup(systemd 서비스 tg-load, tg-canary)으로 실행한다. 데몬은 이 둘만 종료 후보로 보고,
# E는 oom_score_adj=-1000이라 보호된다. 디스크는 처리량 약 10.9 MB/s로 제한한다(VM_BPS로 바꿀 수 있다).
#
# 사용법: lab/m4/run.sh --load NAME --daemon MODE [옵션]
#   --load NAME        runaway_growth | spike_mem | burst_mem | normal_burst
#   --daemon MODE      none(데몬 없음) | dry(판단만 기록, 꺼짐 대용) | kill(실제 종료)
#   --id NAME          실행 이름 (기본 <load>-<daemon>)
#   --out DIR          결과 디렉터리 (기본 results/m4/<날짜>/<id>)
#   --cap-sec N        T0 이후 해소를 기다리는 시간 (기본 300)
#   --load-sec N       부하가 스스로 끝나는 시간 (기본 600). 스래싱 중에는 VM이 응답하지 않아 로그를 가져올 수
#                      없으므로, dry-run·none은 부하가 끝나 VM이 돌아온 뒤에 로그를 가져온다
#   --observe-sec N    T0가 없을 때 기다리는 시간 (기본: normal_burst 300, 그 밖 600)
#   --baseline-sec N   부하 전 기준 구간 (기본 15)
#   --pin-mib N        mlock으로 고정할 메모리 (기본 43, 길벗의 회수 불가 메모리 모사. 0이면 쓰지 않는다)
#   --anon-mib N       runaway_growth·spike_mem의 목표 크기 (기본 650)
#   --rate-mib N       runaway_growth의 초당 증가량 (기본 4)
#   --burst-mib N      burst_mem·normal_burst의 덩어리 크기 (기본: burst_mem 500, normal_burst 300)
#   --burst-hold N     덩어리를 유지하는 시간 (기본: burst_mem 20, normal_burst 5)
#   --burst-rest N     해제 후 쉬는 시간 (기본 10)
#
# 부하 크기·버스트 값은 예비 실행으로 맞추기 전의 임시값이다. 실행 조건은 모두 run.json에 남는다.
set -euo pipefail

# M4의 기본 디스크 조건은 처리량 제한이다. config.sh가 읽기 전에 기본값을 정한다.
: "${VM_BPS:=10900000}"
export VM_BPS
source "$(dirname "$0")/../vm/config.sh"

LOAD=""
DAEMON=""
ID=""
OUT=""
CAP_SEC=300
LOAD_SEC=600
OBSERVE_SEC=""
BASELINE_SEC=15
PIN_MIB=43
ANON_MIB=650
RATE_MIB=4
BURST_MIB=""
BURST_HOLD=""
BURST_REST=10
RECV_PORT=9999

while [ $# -gt 0 ]; do
    case "$1" in
        --load) LOAD="$2"; shift 2 ;;
        --daemon) DAEMON="$2"; shift 2 ;;
        --id) ID="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --cap-sec) CAP_SEC="$2"; shift 2 ;;
        --load-sec) LOAD_SEC="$2"; shift 2 ;;
        --observe-sec) OBSERVE_SEC="$2"; shift 2 ;;
        --baseline-sec) BASELINE_SEC="$2"; shift 2 ;;
        --pin-mib) PIN_MIB="$2"; shift 2 ;;
        --anon-mib) ANON_MIB="$2"; shift 2 ;;
        --rate-mib) RATE_MIB="$2"; shift 2 ;;
        --burst-mib) BURST_MIB="$2"; shift 2 ;;
        --burst-hold) BURST_HOLD="$2"; shift 2 ;;
        --burst-rest) BURST_REST="$2"; shift 2 ;;
        *) echo "알 수 없는 옵션: $1" >&2; exit 2 ;;
    esac
done

case "$LOAD" in
    runaway_growth|spike_mem|burst_mem|normal_burst) ;;
    *) echo "--load는 runaway_growth, spike_mem, burst_mem, normal_burst 중 하나여야 한다" >&2; exit 2 ;;
esac
case "$DAEMON" in
    none|dry|kill) ;;
    *) echo "--daemon은 none, dry, kill 중 하나여야 한다" >&2; exit 2 ;;
esac

# 부하별 기본값
case "$LOAD" in
    burst_mem)    : "${BURST_MIB:=500}"; : "${BURST_HOLD:=20}" ;;
    normal_burst) : "${BURST_MIB:=300}"; : "${BURST_HOLD:=5}"; : "${OBSERVE_SEC:=300}" ;;
esac
: "${OBSERVE_SEC:=600}"

for v in CAP_SEC LOAD_SEC OBSERVE_SEC BASELINE_SEC PIN_MIB ANON_MIB RATE_MIB BURST_REST; do
    [[ "${!v}" =~ ^[0-9]+$ ]] || { echo "$v 는 0 이상의 정수여야 한다: ${!v}" >&2; exit 2; }
done
for v in BURST_MIB BURST_HOLD; do
    [ -z "${!v}" ] || [[ "${!v}" =~ ^[0-9]+$ ]] || { echo "$v 는 0 이상의 정수여야 한다: ${!v}" >&2; exit 2; }
done
[ "$LOAD_SEC" -gt 0 ] || { echo "--load-sec는 1 이상이어야 한다" >&2; exit 2; }

ID="${ID:-$LOAD-$DAEMON}"
[[ "$ID" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "--id에는 영문, 숫자, . _ - 만 쓴다" >&2; exit 2; }
OUT="${OUT:-$TG_ROOT/results/m4/$(date +%Y%m%d)/$ID}"
ANALYZE="$TG_ROOT/lab/analyze/m4_analyze.py"

# 부하 인자. 모두 tg-load 모드로 대응한다 (workload/tg_load.c 머리말 참고).
case "$LOAD" in
    runaway_growth) LOAD_ARGS="anon --size-mib $ANON_MIB --rate-mib $RATE_MIB" ;;
    spike_mem)      LOAD_ARGS="anon --size-mib $ANON_MIB --rate-mib 0" ;;
    burst_mem|normal_burst)
        LOAD_ARGS="burst --size-mib $BURST_MIB --hold $BURST_HOLD --rest $BURST_REST" ;;
esac

mkdir -p "$OUT"
log "결과 디렉터리: $OUT"
log "부하=$LOAD ($LOAD_ARGS), 데몬=$DAEMON, 디스크 처리량 ${VM_BPS} B/s"

pids=()
cleanup() {
    for p in "${pids[@]}"; do kill -INT "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
}
trap 'cleanup; "$VM_DIR/stop.sh" --force >/dev/null 2>&1 || true' EXIT

# VM 안 명령은 멈출 수 있으므로 항상 시간 제한을 둔다.
vm_run() {
    local limit="$1"; shift
    timeout "$limit" ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" "$@"
}

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

vm_scp "$TG_ROOT/build/glibc/tg-sampler" "$TG_ROOT/build/glibc/tg-load" \
    "$TG_ROOT/build/glibc/thrash-guard" "$TG_ROOT/packaging/thrash-guard.service" /tmp/
vm_run 30 "sudo nohup /tmp/tg-sampler --dest $GUEST_HOST_IP:$RECV_PORT --interval-ms 250 \
        >/tmp/sampler.out 2>&1 </dev/null &"
sleep 1
vm_run 30 'pgrep -x tg-sampler >/dev/null' \
    || { echo "tg-sampler가 시작되지 않았다" >&2; vm_run 30 'cat /tmp/sampler.out' >&2 || true; exit 1; }

# VM 벽시계와 수신기 시계의 차이. 데몬 로그의 시각을 수신기 시계로 맞출 때 쓴다(guest - host).
clock_offset_ns() {
    local t1 g t2
    t1="$(date +%s%N)"
    g="$(vm_run 30 'date +%s%N')"
    t2="$(date +%s%N)"
    echo $(( g - (t1 + t2) / 2 ))
}
CLOCK_OFFSET_START_NS="$(clock_offset_ns)"
log "VM 시계 차이(guest - host): $((CLOCK_OFFSET_START_NS / 1000000)) ms"

# 데몬. 종료 후보는 부하(tg-load)와 E(tg-canary) 두 서비스뿐이다. E는 보호되어 후보에서 빠진다.
if [ "$DAEMON" != none ]; then
    flag="--dry-run"
    [ "$DAEMON" = kill ] && flag="--no-dry-run"
    vm_run 30 'sudo install -d /etc/thrash-guard && sudo tee /etc/thrash-guard/thrash-guard.conf >/dev/null' <<'EOF'
target_parent = /sys/fs/cgroup/system.slice
target_match = tg-load.service,tg-canary.service
EOF
    vm_run 60 "sudo bash -s -- '$flag'" <<'EOF'
set -e
install -m0755 /tmp/thrash-guard /usr/local/sbin/thrash-guard
sed "s/ --dry-run\$/ $1/" /tmp/thrash-guard.service > /etc/systemd/system/thrash-guard.service
systemctl daemon-reload
systemctl start thrash-guard
EOF
    sleep 3
    vm_run 30 'systemctl is-active thrash-guard >/dev/null' \
        || { echo "데몬이 시작되지 않았다" >&2; vm_run 30 'sudo journalctl -u thrash-guard --no-pager | tail -20' >&2 || true; exit 1; }
    vm_run 30 'grep ExecStart= /etc/systemd/system/thrash-guard.service' >"$OUT/daemon_exec.txt"
    log "데몬 시작: $(cat "$OUT/daemon_exec.txt")"
fi

# 필수 프로세스 E: 응답 서버(카나리). oom_score_adj=-1000으로 보호한다.
vm_run 30 "sudo systemd-run -q --unit=tg-canary -p OOMScoreAdjust=-1000 \
        /tmp/tg-load canary --port $CANARY_PORT --work-kib 4096 --duration $((LOAD_SEC + BASELINE_SEC + 600))"
sleep 2
vm_run 30 'systemctl is-active tg-canary >/dev/null' \
    || { echo "카나리(E)가 시작되지 않았다" >&2; vm_run 30 'sudo journalctl -u tg-canary --no-pager | tail' >&2 || true; exit 1; }
python3 "$TG_ROOT/lab/collect/canary_probe.py" --port "$CANARY_PORT" --out "$OUT/canary_probe.tsv" \
    2>"$OUT/canary_probe.err" &
pids+=($!)

# 회수 불가 메모리는 부하가 아니라 "원래 있던 메모리"로 보이게 기준 구간 전에 고정한다.
if [ "$PIN_MIB" -gt 0 ]; then
    vm_run 30 "sudo systemd-run -q --unit=tg-pin \
            /tmp/tg-load pin --size-mib $PIN_MIB --duration $((LOAD_SEC + BASELINE_SEC + 600))"
    sleep 2
    vm_run 30 'systemctl is-active tg-pin >/dev/null' \
        || { echo "pin이 시작되지 않았다" >&2; vm_run 30 'sudo journalctl -u tg-pin --no-pager | tail' >&2 || true; exit 1; }
fi

log "기준 구간 ${BASELINE_SEC}초"
sleep "$BASELINE_SEC"

LOAD_START_NS="$(date +%s%N)"
log "부하 시작: $LOAD"
vm_run 30 "sudo systemd-run -q --unit=tg-load \
        -p StandardOutput=file:/tmp/tg-load.out -p StandardError=file:/tmp/tg-load.out \
        /tmp/tg-load $LOAD_ARGS --duration $LOAD_SEC"
sleep 2
# 부하가 실제로 걸리지 않았는데 결과가 "스래싱 미발생"으로 기록되는 일을 막는다.
vm_run 30 'systemctl is-active tg-load >/dev/null' \
    || { echo "부하(tg-load)가 시작되지 않았다" >&2; vm_run 30 'cat /tmp/tg-load.out' >&2 || true; exit 1; }

# 실행 조건 기록. 계산 결과는 끝난 뒤 result.json에 합친다.
RUN_ID="$ID" LOAD="$LOAD" LOAD_ARGS="$LOAD_ARGS" DAEMON="$DAEMON" CAP_SEC="$CAP_SEC" LOAD_SEC="$LOAD_SEC" \
OBSERVE_SEC="$OBSERVE_SEC" BASELINE_SEC="$BASELINE_SEC" PIN_MIB="$PIN_MIB" \
IO_IOPS="$VM_IOPS" IO_BPS="$VM_BPS" LOAD_START_NS="$LOAD_START_NS" OFFSET_NS="$CLOCK_OFFSET_START_NS" \
python3 - "$OUT/run.json" <<'EOF'
import json, os, sys
e = os.environ
json.dump({
    "id": e["RUN_ID"], "load": e["LOAD"], "load_args": e["LOAD_ARGS"], "daemon": e["DAEMON"],
    "cap_sec": int(e["CAP_SEC"]), "load_sec": int(e["LOAD_SEC"]), "observe_sec": int(e["OBSERVE_SEC"]),
    "baseline_sec": int(e["BASELINE_SEC"]), "pin_mib": int(e["PIN_MIB"]),
    "io_limit": {"iops": int(e["IO_IOPS"]), "bytes_per_sec": int(e["IO_BPS"])},
    "load_start_unix_ns": int(e["LOAD_START_NS"]), "guest_clock_offset_ns": int(e["OFFSET_NS"]),
}, open(sys.argv[1], "w"), ensure_ascii=False, indent=2)
EOF

# 5초마다 판정한다. 확정되면 바로 끝낸다 (resolved, unresolved, truncated, no_thrash).
deadline=$((SECONDS + LOAD_SEC + 180))
status=running
while [ "$SECONDS" -lt "$deadline" ]; do
    sleep 5
    status="$(python3 "$ANALYZE" "$OUT" --now-ns "$(date +%s%N)" --field status)"
    [ "$status" = running ] || break
done
log "판정: $status"
sleep 5   # 해소 직후의 프로브와 샘플이 모이도록 잠깐 더 받는다
END_NS="$(date +%s%N)"

# 스래싱 중에는 VM이 응답하지 않는다. 부하가 스스로 끝나(--duration) VM이 돌아오면 로그를 가져온다.
# 끝내 응답하지 않으면 로그 없이 마친다(데몬 판단 시각은 알 수 없다).
reachable=0
fetch_deadline=$(( LOAD_START_NS / 1000000000 + LOAD_SEC + 90 ))
while [ "$(date +%s)" -lt "$fetch_deadline" ]; do
    if timeout 20 ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" true 2>/dev/null; then
        reachable=1
        break
    fi
    sleep 10
done

if [ "$reachable" = 1 ]; then
    log "VM 응답: 로그 수집"
    vm_run 60 'sudo journalctl -u thrash-guard --no-pager -o cat 2>/dev/null' >"$OUT/daemon.log" || true
    vm_run 60 'sudo dmesg | grep -a -i -E "out of memory|oom-kill|killed process" || true' >"$OUT/dmesg_oom.txt" || true
    vm_run 60 'tail -n 30 /tmp/tg-load.out 2>/dev/null; true' >"$OUT/load.out" || true
    vm_run 60 'echo "$(systemctl is-active tg-canary)"; systemctl show tg-canary tg-load -p Id -p ActiveState -p SubState -p Result' \
        >"$OUT/e_state.txt" 2>&1 || true
    CLOCK_OFFSET_END_NS="$(clock_offset_ns 2>/dev/null || echo "")"
    [ -z "$CLOCK_OFFSET_END_NS" ] || echo "{\"start_ns\": $CLOCK_OFFSET_START_NS, \"end_ns\": $CLOCK_OFFSET_END_NS}" >"$OUT/clock_offset.json"
else
    log "VM이 응답하지 않아 로그를 가져오지 못했다"
    echo "VM이 부하 종료 후에도 응답하지 않아 데몬 로그, dmesg, E 상태를 가져오지 못했다" >"$OUT/fetch_failed.txt"
fi

cleanup
pids=()
python3 "$TG_ROOT/lab/collect/tg_to_csv.py" "$OUT/samples.log" --out "$OUT/samples.csv" || true
"$VM_DIR/stop.sh" --force >/dev/null 2>&1 || true

python3 "$ANALYZE" "$OUT" --now-ns "$END_NS" --final >"$OUT/analysis.json"
python3 - "$OUT" <<'EOF'
import json, sys
d = sys.argv[1]
run = json.load(open(f"{d}/run.json"))
res = json.load(open(f"{d}/analysis.json"))
json.dump({**run, "result": res}, open(f"{d}/result.json", "w"), ensure_ascii=False, indent=2)
e = res["e_canary"]
print(f"결과: {res['status']}  (T0 {res['t0_s']}초, 해소 시간 {res['resolve_time_s']}초, "
      f"데몬 결정 {res['t_detect_s']}초, 종료 {res['t_kill_s']}초, 커널 OOM {res['kernel_oom_kills']}건, "
      f"E 최대 무응답 {e['max_gap_s']}초, 샘플 {res['samples']}개, 무효 구간 {res['invalid_buckets']}개)")
EOF
log "완료: $OUT"
