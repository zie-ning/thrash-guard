#!/bin/bash
# M0 완료 조건 점검
#
# VM에 stress-ng로 메모리 압력을 줬을 때, VM 밖(WSL)에 다음이 같은 시간축으로 남는지 확인한다.
#   - tg-sampler가 UDP로 보낸 PSI·vmstat (samples.log → samples.csv)
#   - 외부 SSH 프로브 지연 (ssh_probe.tsv)
#
# 사용법: lab/m0_check.sh [결과 디렉터리]
set -euo pipefail
source "$(dirname "$0")/vm/config.sh"

out="${1:-$TG_ROOT/results/m0-check/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$out"
RECV_PORT=9999
STRESS_SEC="${STRESS_SEC:-60}"
# stress-ng의 % 값은 "남은 메모리" 기준이며 작업자 수로 나눠진다. 압력을 만들려면 100%에 가까워야 한다.
STRESS_BYTES="${STRESS_BYTES:-98%}"

log "결과 디렉터리: $out"
make -C "$TG_ROOT" -s

"$VM_DIR/reset-run.sh"
"$TG_ROOT/lab/collect/run_meta.sh" "$out" >/dev/null

pids=()
cleanup() {
    for p in "${pids[@]}"; do kill -INT "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
}
trap cleanup EXIT

python3 "$TG_ROOT/lab/collect/tg_receiver.py" --port "$RECV_PORT" --out "$out/samples.log" \
    2>"$out/receiver.err" &
pids+=($!)
python3 "$TG_ROOT/lab/collect/ssh_probe.py" --port "$SSH_PORT" --out "$out/ssh_probe.tsv" \
    2>"$out/probe.err" &
pids+=($!)

vm_scp "$TG_ROOT/build/glibc/tg-sampler" /tmp/
vm_ssh "sudo nohup /tmp/tg-sampler --dest $GUEST_HOST_IP:$RECV_PORT --interval-ms 250 \
        >/tmp/sampler.out 2>&1 </dev/null &"

log "기준 구간 15초"
sleep 15
log "stress-ng ${STRESS_SEC}초 (익명 메모리 ${STRESS_BYTES})"
vm_ssh "sudo nohup stress-ng --vm 2 --vm-bytes $STRESS_BYTES --vm-keep --timeout ${STRESS_SEC}s \
        >/tmp/stress.out 2>&1 </dev/null &" || true
sleep "$STRESS_SEC"
log "회복 구간 20초"
sleep 20

vm_ssh 'cat /tmp/sampler.out; sudo pkill -INT tg-sampler' >"$out/sampler.out" 2>&1 || true
vm_ssh 'cat /tmp/stress.out; sudo dmesg | grep -i -E "oom|killed process" || true' \
    >"$out/stress.out" 2>&1 || true
sleep 1
cleanup
trap - EXIT

python3 "$TG_ROOT/lab/collect/tg_to_csv.py" "$out/samples.log" --out "$out/samples.csv"

# 간단 요약
python3 - "$out" <<'EOF'
import csv, sys
d = sys.argv[1]
rows = list(csv.DictReader(open(f"{d}/samples.csv")))
probe = list(csv.DictReader(open(f"{d}/ssh_probe.tsv"), delimiter="\t"))
seqs = [int(r["seq"]) for r in rows]
missing = (max(seqs) - min(seqs) + 1 - len(seqs)) if seqs else 0
print(f"샘플 {len(rows)}개, 누락 {missing}개")
print(f"PSI some avg10 최대 {max(int(r['psi_some_avg10']) for r in rows)/100:.2f}%")
print(f"PSI full avg10 최대 {max(int(r['psi_full_avg10']) for r in rows)/100:.2f}%")
# avg10은 지수 평균이라 짧은 stall이 묻힌다. 누적값(total, 마이크로초)의 차분으로 직접 계산한다.
def stall_pct(key):
    best = 0.0
    for a, b in zip(rows, rows[1:]):
        dt = int(b["mono_us"]) - int(a["mono_us"])
        if dt > 0:
            best = max(best, (int(b[key]) - int(a[key])) / dt * 100)
    return best
some_ms = (int(rows[-1]["psi_some_total"]) - int(rows[0]["psi_some_total"])) / 1000
full_ms = (int(rows[-1]["psi_full_total"]) - int(rows[0]["psi_full_total"])) / 1000
print(f"PSI 누적 stall 증가: some {some_ms:.0f} ms, full {full_ms:.0f} ms")
print(f"PSI 샘플 구간(250ms) 최대 stall 비율: some {stall_pct('psi_some_total'):.1f}%, full {stall_pct('psi_full_total'):.1f}%")
print(f"OOM kill {int(rows[-1]['oom_kill']) - int(rows[0]['oom_kill'])}회, 재적재(file) {int(rows[-1]['refault_file']) - int(rows[0]['refault_file'])}페이지")
print(f"MemAvailable 최소 {min(int(r['mem_avail_kb']) for r in rows)/1024:.1f} MiB")
print(f"샘플러 late_us 최대 {max(int(r['late_us']) for r in rows)/1000:.1f} ms")
lat = [float(p["latency_ms"]) for p in probe]
fails = sum(1 for p in probe if p["ok"] != "1")
print(f"SSH 프로브 {len(probe)}회, 실패 {fails}회, 지연 최대 {max(lat):.0f} ms" if lat else "SSH 프로브 기록 없음")
EOF
log "완료: $out"
