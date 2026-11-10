#!/bin/bash
# lab/vm 공용 설정 (D1: Windows QEMU + WHPX, WSL에서 조종)
#
# WSL은 mirrored 네트워크 모드여야 한다. 그래야 WSL의 127.0.0.1이 Windows와 공유되어
# QEMU의 SSH 포워딩·QMP 포트에 바로 접속할 수 있다.

TG_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VM_DIR="$TG_ROOT/lab/vm"
IMG_DIR="$VM_DIR/images"

# Windows 쪽 QEMU 실행 파일 (WSL interop으로 실행)
QEMU_DIR="${QEMU_DIR:-/mnt/c/Program Files/qemu}"
QEMU_EXE="$QEMU_DIR/qemu-system-x86_64.exe"
QEMU_IMG="$QEMU_DIR/qemu-img.exe"

# 원본 클라우드 이미지와 기준 이미지
CLOUD_IMG="$IMG_DIR/ubuntu-22.04-server-cloudimg-amd64.img"
BASE_IMG="$IMG_DIR/base-jammy-hwe.qcow2"   # 프로비저닝을 마친 기준 이미지
RUN_IMG="$IMG_DIR/run.qcow2"               # 실행마다 새로 만드는 일회용 오버레이
DISK_SIZE="10G"

# VM 사양 (길벗 t3.micro에 맞춘 값)
VM_MEM_MB="${VM_MEM_MB:-1024}"
VM_CPUS="${VM_CPUS:-2}"

# 디스크 I/O 상한 (기본: EBS gp3 기본 성능). 0이면 제한하지 않는다.
VM_IOPS="${VM_IOPS:-3000}"
VM_BPS="${VM_BPS:-131072000}"   # 125 MiB/s

# 포트 (모두 127.0.0.1에만 연다)
SSH_PORT="${SSH_PORT:-2222}"
QMP_PORT="${QMP_PORT:-4444}"
SEED_PORT="${SEED_PORT:-8000}"   # cloud-init 설정을 건네는 임시 HTTP 서버

# VM 안에서 본 호스트 주소 (QEMU user 네트워크의 고정값)
GUEST_HOST_IP="10.0.2.2"

# SSH
VM_USER="tg"
SSH_KEY="$HOME/.ssh/tg_vm_ed25519"
SSH_OPTS=(-i "$SSH_KEY" -p "$SSH_PORT" -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5)

vm_ssh() {
    ssh "${SSH_OPTS[@]}" "$VM_USER@127.0.0.1" "$@"
}

vm_scp() {
    # 사용법: vm_scp <로컬 파일...> <VM 안 경로>
    local dest="${*: -1}"
    local srcs=("${@:1:$#-1}")
    scp -i "$SSH_KEY" -P "$SSH_PORT" -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "${srcs[@]}" "$VM_USER@127.0.0.1:$dest"
}

# WSL 경로를 Windows 경로로 바꾼다 (QEMU.exe 인자용)
winpath() {
    wslpath -w "$1"
}

log() {
    printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >&2
}

# QEMU가 스스로 끝날 때까지 기다리고, 시간이 지나면 강제 종료한다. 사용법: wait_qemu_exit <초>
wait_qemu_exit() {
    local limit="$1" t=0
    while python3 "$VM_DIR/qmp.py" status "$QMP_PORT" >/dev/null 2>&1; do
        sleep 2
        t=$((t + 2))
        if [ "$t" -ge "$limit" ]; then
            python3 "$VM_DIR/qmp.py" quit "$QMP_PORT" >/dev/null 2>&1 || true
            sleep 2
            return 1
        fi
    done
}

# SSH가 응답할 때까지 기다린다. 사용법: wait_ssh <초>
wait_ssh() {
    local limit="$1" t=0
    while ! vm_ssh true 2>/dev/null; do
        sleep 3
        t=$((t + 3))
        if [ "$t" -ge "$limit" ]; then
            return 1
        fi
    done
}
