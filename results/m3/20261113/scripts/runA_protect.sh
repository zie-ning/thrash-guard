#!/bin/bash
# M3 VM 검증 1회: 모드(dry|kill)를 받아 VM을 새로 켜고 스래싱 부하를 걸어 데몬 동작을 관찰한다.
set -u
MODE=kill
PROT="${1:-protected}"   # protected | plain
PFLAG=""; [ "$PROT" = protected ] && PFLAG="--oom-score-adj=-1000"
WT=/mnt/c/thrash-guard/.claude/worktrees/m3-implementation-scope-822da8
export VM_IOPS="${VM_IOPS:-100}"
source /mnt/c/thrash-guard/lab/vm/config.sh
OUT="${2:-/tmp/m3runA.$PROT.log}"
: > "$OUT"
say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT"; }
q() { timeout "${T:-15}" ssh "${SSH_OPTS[@]}" -o ConnectTimeout=5 tg@127.0.0.1 "$@" 2>&1; }

say "실험 A: prot=$PROT, VM 시작 (IOPS=$VM_IOPS)"
/mnt/c/thrash-guard/lab/vm/reset-run.sh >/dev/null 2>&1 || { say "VM 시작 실패"; exit 1; }
T=60 vm_scp /tmp/psirec "$WT/build/glibc/thrash-guard" "$WT/build/glibc/tg-load" "$WT/packaging/thrash-guard.service" "$WT/packaging/thrash-guard.conf" /tmp/ || exit 1
EXEC="--dry-run"; [ "$MODE" = kill ] && EXEC="--no-dry-run"
T=60 q "sudo install -m0755 /tmp/thrash-guard /usr/local/sbin/ && sudo install -D -m0644 /tmp/thrash-guard.conf /etc/thrash-guard/thrash-guard.conf && sed 's/ --dry-run\$/ $EXEC/' /tmp/thrash-guard.service | sudo tee /etc/systemd/system/thrash-guard.service >/dev/null && sudo systemctl daemon-reload && mkdir -p /tmp/img && cp /tmp/tg-load /tmp/img/ && tar -C /tmp/img -c tg-load | sudo docker import - tgload >/dev/null && sudo systemctl start thrash-guard; sudo systemd-run -q --unit=tg-psirec /tmp/psirec /dev/shm/psi.tsv 900; sudo systemd-run -q --unit=tg-pin /tmp/tg-load pin --size-mib 43 --duration 900; sleep 3; systemctl is-active thrash-guard; grep ExecStart /etc/systemd/system/thrash-guard.service" | tee -a "$OUT"
q "sudo docker run -d --name prot $PFLAG --entrypoint /tg-load tgload anon --size-mib 450 --rate-mib 2 --duration 420 >/dev/null && sudo docker run -d --name hog --entrypoint /tg-load tgload anon --size-mib 250 --rate-mib 4 --duration 420 >/dev/null && echo both-started; for c in prot hog; do echo \$c oom_score_adj=\$(cat /proc/\$(sudo docker inspect -f {{.State.Pid}} \$c)/oom_score_adj); done" | tee -a "$OUT"
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
    echo "$r" | grep -q -E "(hog|prot):Exited" && { say "컨테이너 종료 감지"; break; }
done
sleep 20
kill $PROBE 2>/dev/null
say "==== 데몬 로그"
T=40 q 'sudo journalctl -u thrash-guard --no-pager -o cat | grep -a -v "^  " | tail -60; sudo dmesg 2>/dev/null | grep -a -i "out of memory\|oom-kill\|Killed process" | tail -5; echo; echo pressure; cat /proc/pressure/memory; sudo docker ps -a --format "{{.Names}} {{.Status}}"; sudo systemctl stop thrash-guard; sudo cp /dev/shm/psi.tsv /var/tmp/psi.tsv; sudo journalctl -u thrash-guard --no-pager -o cat | grep -a stats' | tee -a "$OUT"
T=90 q "sudo cat /dev/shm/psi.tsv" > "$OUT.psi"
say "psi 기록 $(wc -l < "$OUT.psi")줄"
