#!/bin/bash
# Ubuntu 22.04 클라우드 이미지로 기준 이미지(base-jammy-hwe.qcow2)를 만든다. (M0-6)
#
# 1. 클라우드 이미지 위에 작업용 오버레이를 만들고 10G로 늘린다
# 2. cloud-init 설정(사용자, SSH 키)을 임시 HTTP 서버로 건네며 최초 부팅
# 3. provision.sh로 HWE 6.8 커널, Docker, workload.slice 설치
# 4. 재부팅해 6.8 커널로 올라왔는지 verify.sh로 확인하고 결과를 base-info.txt에 저장
# 5. cloud-init을 끄고 종료한 뒤, 오버레이를 독립된 기준 이미지로 변환
#
# 이후 실험은 이 기준 이미지 위에 일회용 오버레이를 만들어 시작한다(reset-run.sh).
set -euo pipefail
source "$(dirname "$0")/config.sh"

[ -f "$CLOUD_IMG" ] || { echo "클라우드 이미지가 없다: $CLOUD_IMG" >&2; exit 1; }

if [ ! -f "$SSH_KEY" ]; then
    log "VM 접속용 SSH 키 생성: $SSH_KEY"
    mkdir -p "$(dirname "$SSH_KEY")"
    ssh-keygen -q -t ed25519 -N "" -C "thrash-guard-vm" -f "$SSH_KEY"
fi

build="$IMG_DIR/build.qcow2"
seed="$IMG_DIR/seed"
rm -f "$build"
"$QEMU_IMG" create -q -f qcow2 -F qcow2 -b "$(winpath "$CLOUD_IMG")" "$(winpath "$build")" "$DISK_SIZE"

# cloud-init NoCloud 설정
mkdir -p "$seed"
cat >"$seed/meta-data" <<EOF
instance-id: tg-base
local-hostname: tg-vm
EOF
cat >"$seed/user-data" <<EOF
#cloud-config
users:
  - name: $VM_USER
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys:
      - $(cat "$SSH_KEY.pub")
ssh_pwauth: false
EOF
: >"$seed/vendor-data"

python3 -m http.server "$SEED_PORT" --bind 127.0.0.1 --directory "$seed" >"$IMG_DIR/seed-http.log" 2>&1 &
http_pid=$!
cleanup() { kill "$http_pid" 2>/dev/null || true; }
trap cleanup EXIT

"$VM_DIR/start.sh" "$build" --no-throttle --seed

log "SSH 대기 (최대 10분)"
wait_ssh 600 || { echo "SSH 접속 실패. $IMG_DIR/serial.log 확인" >&2; exit 1; }
vm_ssh 'cloud-init status --wait' || true
cleanup

log "프로비저닝"
vm_scp "$VM_DIR/provision.sh" "$VM_DIR/verify.sh" /tmp/
vm_ssh 'sudo bash /tmp/provision.sh' >"$IMG_DIR/provision.log" 2>&1 \
    || { echo "프로비저닝 실패. $IMG_DIR/provision.log 확인" >&2; exit 1; }

log "재시작 (6.8 커널로)"
# WHPX에서 게스트 재부팅이 실패하므로 끄고 QEMU를 새로 켠다.
vm_ssh 'sudo systemctl poweroff' || true
wait_qemu_exit 120 || log "정상 종료 시간 초과, 강제 종료함"
"$VM_DIR/start.sh" "$build" --no-throttle
wait_ssh 300 || { echo "재시작 후 SSH 접속 실패. $IMG_DIR/serial.log 확인" >&2; exit 1; }

vm_scp "$VM_DIR/verify.sh" /tmp/
vm_ssh 'sudo bash /tmp/verify.sh' | tee "$IMG_DIR/base-info.txt"

log "종료"
vm_ssh 'sudo touch /etc/cloud/cloud-init.disabled && sudo systemctl poweroff' || true
wait_qemu_exit 120 || log "정상 종료 시간 초과, 강제 종료함"

log "기준 이미지로 변환: $(basename "$BASE_IMG")"
rm -f "$BASE_IMG"
"$QEMU_IMG" convert -O qcow2 "$(winpath "$build")" "$(winpath "$BASE_IMG")"
rm -f "$build"
ls -lh "$BASE_IMG"
log "완료"
