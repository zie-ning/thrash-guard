#!/bin/bash
# M3 VM 검증 1회: 모드(dry|kill)를 받아 VM을 새로 켜고 스래싱 부하를 걸어 데몬 동작을 관찰한다.
set -u
MODE="${1:-kill}"
WT=/mnt/c/thrash-guard/.claude/worktrees/m3-implementation-scope-822da8
export VM_IOPS="${VM_IOPS:-100}"
source /mnt/c/thrash-guard/lab/vm/config.sh
OUT="${2:-/tmp/m3run.$MODE.log}"
: > "$OUT"
say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT"; }
q() { timeout "${T:-15}" ssh "${SSH_OPTS[@]}" -o ConnectTimeout=5 tg@127.0.0.1 "$@" 2>&1; }

say "VM 시작 (IOPS=$VM_IOPS)"
/mnt/c/thrash-guard/lab/vm/reset-run.sh >/dev/null 2>&1 || { say "VM 시작 실패"; exit 1; }
T=60 vm_scp "$WT/build/glibc/thrash-guard" "$WT/build/glibc/tg-load" "$WT/packaging/thrash-guard.service" "$WT/packaging/thrash-guard.conf" /tmp/ || exit 1
EXEC="--dry-run"; [ "$MODE" = kill ] && EXEC="--no-dry-run"
T=60 q "sudo install -m0755 /tmp/thrash-guard /usr/local/sbin/ && sudo install -D -m0644 /tmp/thrash-guard.conf /etc/thrash-guard/thrash-guard.conf && sed 's/ --dry-run\$/ $EXEC/' /tmp/thrash-guard.service | sudo tee /etc/systemd/system/thrash-guard.service >/dev/null && sudo systemctl daemon-reload && mkdir -p /tmp/img && cp /tmp/tg-load /tmp/img/ && tar -C /tmp/img -c tg-load | sudo docker import - tgload >/dev/null && sudo systemctl start thrash-guard; sudo systemd-run -q --unit=tg-pin /tmp/tg-load pin --size-mib 43 --duration 900; sleep 3; systemctl is-active thrash-guard; grep ExecStart /etc/systemd/system/thrash-guard.service" | tee -a "$OUT"
q "sudo docker run -d --name prot --oom-score-adj=-1000 --entrypoint /tg-load tgload anon --size-mib 450 --rate-mib 2 --duration 420 >/dev/null && sudo docker run -d --name hog --entrypoint /tg-load tgload anon --size-mib 250 --rate-mib 4 --duration 420 >/dev/null && echo both-started" | tee -a "$OUT"
( while true; do
    if timeout 3 bash -c 'read -t 3 l < /dev/tcp/127.0.0.1/2222 && [ -n "$l" ]' 2>/dev/null; then echo "$(date +%s) ok"; else echo "$(date +%s) fail"; fi
    sleep 1
  done > "$OUT.probe" ) &
PROBE=$!
for i in $(seq 1 60); do
    sleep 10
    r=$(T=12 q 'echo $(grep -E "^MemAvailable" /proc/meminfo | tr -s " " | cut -d" " -f2)kB $(grep full /proc/pressure/memory | cut -d" " -f2) $(sudo docker ps -a --format "{{.Names}}:{{.Status}}" | tr "
" " ") ; sudo journalctl -u thrash-guard --no-pager -o cat | grep -a -c "judge kill"')
    say "t=$((i*10)) $(echo $r | tr '\n' ' ')"
    echo "$r" | grep -q "hog:Exited" && { say "hog 종료 감지"; break; }
done
sleep 20
kill $PROBE 2>/dev/null
say "==== 데몬 로그"
T=40 q 'sudo journalctl -u thrash-guard --no-pager -o cat | grep -a -v "^  " | tail -40; sudo dmesg 2>/dev/null | grep -a -i "out of memory\|oom-kill\|Killed process" | tail -5; echo; echo pressure; cat /proc/pressure/memory; sudo docker ps -a --format "{{.Names}} {{.Status}}"; sudo systemctl stop thrash-guard; sudo journalctl -u thrash-guard --no-pager -o cat | grep -a stats' | tee -a "$OUT"
