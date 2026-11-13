#!/bin/bash
# 실험 B: sshd 보호(MemoryLow)가 스래싱 중 SSH 응답을 지키는가.
# 사용법: m3runB.sh <none|chain> <출력 경로>
#   none  : MemoryLow 없음
#   chain : ssh.service, system.slice, user.slice에 각각 32M
# 데몬은 dry-run(종료 효과를 빼기 위해). 부하는 M1 기준 단일 hog.
set -u
ARM="${1:?none|chain}"
OUT="${2:?출력 경로}"
WT=/mnt/c/thrash-guard/.claude/worktrees/m3-implementation-scope-822da8
export VM_IOPS="${VM_IOPS:-100}"
source /mnt/c/thrash-guard/lab/vm/config.sh
: > "$OUT"
say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT"; }
q() { timeout "${T:-15}" ssh "${SSH_OPTS[@]}" -o ConnectTimeout=5 tg@127.0.0.1 "$@" 2>&1; }

# 로그인해서 가벼운 명령을 실행하고 걸린 시간을 잰다 (sudo 없이)
STATUS_CMD='echo avail=$(awk "/MemAvailable/{print int(\$2/1024)}" /proc/meminfo)MiB full=$(grep full /proc/pressure/memory | cut -d" " -f2) containers=$(ls -d /sys/fs/cgroup/system.slice/docker-*.scope 2>/dev/null | wc -l)'
login_probe() {
    local s e out rc
    s=$(date +%s.%N)
    out=$(timeout 30 ssh "${SSH_OPTS[@]}" -o ConnectTimeout=25 tg@127.0.0.1 "$STATUS_CMD" 2>&1)
    rc=$?
    e=$(date +%s.%N)
    printf '%s %s %d | %s\n' "$s" "$(awk -v a="$s" -v b="$e" 'BEGIN{printf "%.1f", b-a}')" "$rc" "$(echo $out | tr '\n' ' ')"
}

say "실험 B: arm=$ARM, VM 시작 (IOPS=$VM_IOPS)"
/mnt/c/thrash-guard/lab/vm/reset-run.sh >/dev/null 2>&1 || { say "VM 시작 실패"; exit 1; }
T=60 vm_scp /tmp/psirec "$WT/build/glibc/thrash-guard" "$WT/build/glibc/tg-load" "$WT/packaging/thrash-guard.service" "$WT/packaging/thrash-guard.conf" /tmp/ || exit 1
T=90 q "sudo install -m0755 /tmp/thrash-guard /usr/local/sbin/ && sudo install -D -m0644 /tmp/thrash-guard.conf /etc/thrash-guard/thrash-guard.conf && sudo install -m0644 /tmp/thrash-guard.service /etc/systemd/system/ && sudo systemctl daemon-reload && mkdir -p /tmp/img && cp /tmp/tg-load /tmp/img/ && tar -C /tmp/img -c tg-load | sudo docker import - tgload >/dev/null && sudo systemctl start thrash-guard; sudo systemd-run -q --unit=tg-psirec /tmp/psirec /dev/shm/psi.tsv 900; sudo systemd-run -q --unit=tg-pin /tmp/tg-load pin --size-mib 43 --duration 900; sleep 3; systemctl is-active thrash-guard; grep ExecStart /etc/systemd/system/thrash-guard.service" | tee -a "$OUT"

if [ "$ARM" = chain ]; then
    T=60 q "sudo systemctl set-property --runtime ssh.service MemoryLow=32M && sudo systemctl set-property --runtime system.slice MemoryLow=32M && sudo systemctl set-property --runtime user.slice MemoryLow=32M" | tee -a "$OUT"
fi
T=30 q 'for u in system.slice system.slice/ssh.service user.slice; do echo "$u memory.low=$(cat /sys/fs/cgroup/$u/memory.low) current=$(( $(cat /sys/fs/cgroup/$u/memory.current)/1048576 ))MiB"; done' | tee -a "$OUT"

T=60 q "sudo docker run -d --name hog --entrypoint /tg-load tgload anon --size-mib 650 --rate-mib 4 --duration 420 >/dev/null && echo hog-started" | tee -a "$OUT"

# 배너 감시 (1~4초 간격)
( while true; do
    if timeout 3 bash -c 'read -t 3 l < /dev/tcp/127.0.0.1/2222 && [ -n "$l" ]' 2>/dev/null; then echo "$(date +%s) ok"; else echo "$(date +%s) fail"; fi
    sleep 1
  done > "$OUT.banner" ) &
BANNER=$!

# 전체 로그인 응답 (이전 시도가 끝나면 10초 뒤 다음 시도)
: > "$OUT.login"
for i in $(seq 1 80); do
    line=$(login_probe)
    echo "$line" >> "$OUT.login"
    say "login #$i $(echo "$line" | cut -c1-150)"
    # 컨테이너가 모두 사라졌고(hog 종료) 한 번이라도 정상 응답했으면 마친다
    echo "$line" | grep -q "containers=0" && { say "hog 종료 감지"; break; }
    sleep 10
done
sleep 20
kill $BANNER 2>/dev/null

say "==== 결과 수집"
T=60 q 'echo "--- memory.events"; for u in system.slice system.slice/ssh.service user.slice user.slice/user-1000.slice; do echo "$u low=$(cat /sys/fs/cgroup/$u/memory.low) :: $(tr "\n" " " < /sys/fs/cgroup/$u/memory.events)"; done; echo "--- oom/dmesg"; sudo dmesg | grep -a -i -E "out of memory|oom-kill|Killed process" | tail -5; echo "--- daemon"; sudo journalctl -u thrash-guard --no-pager -o cat | grep -a -v "^  " | tail -20; sudo systemctl stop thrash-guard; sudo journalctl -u thrash-guard --no-pager -o cat | grep -a stats; echo "--- pressure"; cat /proc/pressure/memory' | tee -a "$OUT"
T=90 q "sudo cat /dev/shm/psi.tsv" > "$OUT.psi"
say "psi 기록 $(wc -l < "$OUT.psi")줄, 완료"
