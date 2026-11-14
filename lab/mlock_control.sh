#!/bin/bash
# 실험 F: 데몬 mlockall 대조군 (이슈 #6). 계획은 docs/validation/mlock-control.md.
#
# 사용법:
#   lab/mlock_control.sh run <mlock|nomlock> <출력 경로>   한 회 실행
#   lab/mlock_control.sh pairs <쌍 수> <출력 디렉터리>      mlock, nomlock을 번갈아 실행 (기본 5쌍)
#
# 조건은 results/m3/20261113/scripts/runC_throughput.sh(처리량 10.9 MB/s, 단일 hog, dry-run)와 같고
# 달라지는 것은 설정 파일의 mlock 값 하나다. 두 팔 모두 로그를 /dev/shm에 쓴다(journald 지연이 섞이지 않게).
# 출력: <경로>(실행 로그), <경로>.psi(14열 기록), <경로>.banner, <경로>.login, <경로>.daemonlog
set -u
TG_LAB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TG_TOP="$(cd "$TG_LAB/.." && pwd)"
export VM_IOPS="${VM_IOPS:-3000}"
export VM_BPS="${VM_BPS:-10900000}"
source "$TG_LAB/vm/config.sh"

run_one() {
    local ARM="${1:?mlock|nomlock}" OUT="${2:?출력 경로}"
    local MLOCK=yes
    [ "$ARM" = nomlock ] && MLOCK=no
    : > "$OUT"
    say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT"; }
    q() { timeout "${T:-15}" ssh "${SSH_OPTS[@]}" -o ConnectTimeout=5 "$VM_USER@127.0.0.1" "$@" 2>&1; }

    local STATUS_CMD='echo avail=$(awk "/MemAvailable/{print int(\$2/1024)}" /proc/meminfo)MiB full=$(grep full /proc/pressure/memory | cut -d" " -f2) containers=$(ls -d /sys/fs/cgroup/system.slice/docker-*.scope 2>/dev/null | wc -l)'
    login_probe() {
        local s e out rc
        s=$(date +%s.%N)
        out=$(timeout 30 ssh "${SSH_OPTS[@]}" -o ConnectTimeout=25 "$VM_USER@127.0.0.1" "$STATUS_CMD" 2>&1)
        rc=$?
        e=$(date +%s.%N)
        printf '%s %s %d | %s\n' "$s" "$(awk -v a="$s" -v b="$e" 'BEGIN{printf "%.1f", b-a}')" "$rc" "$(echo $out | tr '\n' ' ')"
    }

    local BIN="$TG_TOP/build/glibc"
    for f in thrash-guard tg-load; do
        [ -x "$BIN/$f" ] || { say "빌드 산출물이 없다: $BIN/$f (WSL에서 make를 먼저 실행)"; return 1; }
    done
    local PSIREC=/tmp/psirec_mlock
    gcc -O2 -Wall -o "$PSIREC" "$TG_TOP/lab/collect/psirec.c" || { say "psirec 빌드 실패"; return 1; }

    say "실험 F: arm=$ARM (mlock=$MLOCK), VM 시작 (IOPS=$VM_IOPS, BPS=$VM_BPS)"
    "$VM_DIR/reset-run.sh" >/dev/null 2>&1 || { say "VM 시작 실패"; return 1; }
    T=60 vm_scp "$PSIREC" "$BIN/thrash-guard" "$BIN/tg-load" "$TG_TOP/packaging/thrash-guard.service" "$TG_TOP/packaging/thrash-guard.conf" /tmp/ || return 1

    # 설정 파일 끝에 mlock과 log_path를 덧붙인다(뒤의 값이 앞의 값을 덮어쓰는지는 아래에서 시작 로그로 확인한다).
    T=90 q "sudo install -m0755 /tmp/thrash-guard /usr/local/sbin/ && sudo install -D -m0644 /tmp/thrash-guard.conf /etc/thrash-guard/thrash-guard.conf && printf 'mlock = $MLOCK\nlog_path = /dev/shm/tg.log\n' | sudo tee -a /etc/thrash-guard/thrash-guard.conf >/dev/null && sudo install -m0644 /tmp/thrash-guard.service /etc/systemd/system/thrash-guard.service && sudo systemctl daemon-reload && mkdir -p /tmp/img && cp /tmp/tg-load /tmp/img/ && tar -C /tmp/img -c tg-load | sudo docker import - tgload >/dev/null && sudo systemctl start thrash-guard; sleep 2; systemctl is-active thrash-guard; grep ExecStart /etc/systemd/system/thrash-guard.service" | tee -a "$OUT"

    # 적용 확인: 데몬의 VmLck. mlock=true는 0보다 크고 false는 0이어야 한다.
    local DPID VMLCK
    DPID=$(T=15 q "systemctl show -p MainPID --value thrash-guard" | tr -d '[:space:]')
    VMLCK=$(T=15 q "awk '/VmLck/{print \$2}' /proc/$DPID/status" | tr -d '[:space:]')
    say "데몬 PID=$DPID VmLck=${VMLCK}kB (arm=$ARM)"
    if { [ "$ARM" = mlock ] && [ "${VMLCK:-0}" -eq 0 ]; } || { [ "$ARM" = nomlock ] && [ "${VMLCK:-1}" -ne 0 ]; }; then
        say "경고: mlock 설정이 적용되지 않았다. 이 회는 결과에서 제외한다."
    fi

    T=30 q "sudo systemd-run -q --unit=tg-psirec /tmp/psirec_mlock /dev/shm/psi.tsv 900 $DPID; sudo systemd-run -q --unit=tg-pin /tmp/tg-load pin --size-mib 43 --duration 900; sleep 3; echo started" | tee -a "$OUT"
    T=60 q "sudo docker run -d --name hog --entrypoint /tg-load tgload anon --size-mib 650 --rate-mib 4 --duration 420 >/dev/null && echo hog-started" | tee -a "$OUT"

    ( while true; do
        if timeout 3 bash -c 'read -t 3 l < /dev/tcp/127.0.0.1/2222 && [ -n "$l" ]' 2>/dev/null; then echo "$(date +%s) ok"; else echo "$(date +%s) fail"; fi
        sleep 1
      done > "$OUT.banner" ) &
    local BANNER=$!

    : > "$OUT.login"
    local i line
    for i in $(seq 1 80); do
        line=$(login_probe)
        echo "$line" >> "$OUT.login"
        say "login #$i $(echo "$line" | cut -c1-150)"
        echo "$line" | grep -q "containers=0" && { say "hog 종료 감지"; break; }
        sleep 10
    done
    sleep 20
    kill $BANNER 2>/dev/null

    say "==== 결과 수집"
    T=60 q 'echo "--- oom/dmesg"; sudo dmesg | grep -a -i -E "out of memory|oom-kill|Killed process" | tail -5; echo "--- daemon status"; grep -E "VmRSS|VmLck" /proc/'"$DPID"'/status; sudo systemctl stop thrash-guard; echo "--- stats"; sudo grep -a stats /dev/shm/tg.log' | tee -a "$OUT"
    T=90 q "sudo cat /dev/shm/psi.tsv" > "$OUT.psi"
    T=90 q "sudo cat /dev/shm/tg.log" > "$OUT.daemonlog"
    say "psi 기록 $(wc -l < "$OUT.psi")줄, 데몬 로그 $(wc -l < "$OUT.daemonlog")줄, 완료"
}

case "${1:-}" in
run) run_one "${2:-}" "${3:-}" ;;
pairs)
    N="${2:-5}"
    D="${3:?출력 디렉터리}"
    mkdir -p "$D"
    for n in $(seq 1 "$N"); do
        for arm in mlock nomlock; do
            run_one "$arm" "$D/F_mlock_${arm}_$n" > "$D/F_mlock_${arm}_$n.out" 2>&1
        done
    done
    ;;
*) echo "사용법: $0 run <mlock|nomlock> <출력 경로> | pairs <쌍 수> <출력 디렉터리>" >&2; exit 2 ;;
esac
