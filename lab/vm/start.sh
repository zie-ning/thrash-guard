#!/bin/bash
# 실험용 VM을 백그라운드로 켠다.
#
# 사용법: start.sh <디스크 이미지> [--no-throttle] [--seed]
#   --no-throttle  디스크 I/O 상한을 걸지 않는다 (기준 이미지 프로비저닝용)
#   --seed         cloud-init 설정을 HTTP로 건네받도록 SMBIOS에 주소를 넣는다 (최초 부팅용)
#
# 조종 경로
#   SSH  : 127.0.0.1:$SSH_PORT  → VM 22번
#   QMP  : 127.0.0.1:$QMP_PORT  (qmp.py로 리셋·종료)
#   콘솔 : images/serial.log    (커널·부팅 메시지)
set -euo pipefail
source "$(dirname "$0")/config.sh"

img="${1:?디스크 이미지 경로가 필요하다}"
shift
throttle=1
seed=0
for a in "$@"; do
    case "$a" in
        --no-throttle) throttle=0 ;;
        --seed) seed=1 ;;
        *) echo "알 수 없는 옵션: $a" >&2; exit 2 ;;
    esac
done

if python3 "$VM_DIR/qmp.py" status "$QMP_PORT" >/dev/null 2>&1; then
    echo "이미 실행 중인 VM이 있다 (QMP $QMP_PORT). 먼저 종료하라." >&2
    exit 1
fi

drive="file=$(winpath "$img"),if=none,id=d0,format=qcow2"
if [ "$throttle" -eq 1 ] && [ "$VM_IOPS" -gt 0 ]; then
    drive+=",throttling.iops-total=$VM_IOPS,throttling.bps-total=$VM_BPS"
fi

args=(
    -name tg-vm
    -machine q35 -accel whpx
    # WHPX에서는 게스트 재부팅 시 "Unexpected VP exit code 4"로 VM이 일시정지된다.
    # 재부팅 요청이 오면 QEMU를 종료시키고, 다시 켜는 일은 스크립트가 맡는다.
    -no-reboot
    -m "$VM_MEM_MB" -smp "$VM_CPUS"
    -drive "$drive" -device virtio-blk-pci,drive=d0
    -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22"
    -device virtio-net-pci,netdev=n0
    -display none
    -serial "file:$(winpath "$IMG_DIR")\\serial.log"
    -qmp "tcp:127.0.0.1:$QMP_PORT,server,wait=off"
)
if [ "$seed" -eq 1 ]; then
    args+=(-smbios "type=1,serial=ds=nocloud-net;s=http://$GUEST_HOST_IP:$SEED_PORT/")
fi

# 이 셸이 끝나도 QEMU가 살아 있도록 세션을 분리한다.
setsid nohup "$QEMU_EXE" "${args[@]}" >"$IMG_DIR/qemu.log" 2>&1 </dev/null &

for _ in $(seq 1 20); do
    if python3 "$VM_DIR/qmp.py" status "$QMP_PORT" >/dev/null 2>&1; then
        log "VM 시작: $(basename "$img"), 메모리 ${VM_MEM_MB} MiB, vCPU $VM_CPUS, I/O 상한 $([ "$throttle" -eq 1 ] && echo "${VM_IOPS} IOPS / ${VM_BPS} B/s" || echo 없음)"
        exit 0
    fi
    sleep 0.5
done
echo "QEMU가 QMP에 응답하지 않는다. $IMG_DIR/qemu.log 확인:" >&2
cat "$IMG_DIR/qemu.log" >&2
exit 1
