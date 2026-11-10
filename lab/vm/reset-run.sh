#!/bin/bash
# 실험 1회용 VM을 깨끗한 상태로 시작한다.
#
# 기준 이미지 위에 새 오버레이(run.qcow2)를 만들어 켠다. 실행 중 디스크에 생긴 변화는
# 오버레이에만 쌓이므로, 다음 실행 때 오버레이를 지우면 기준 상태로 돌아간다.
# 이미 VM이 켜져 있으면 강제로 끈 뒤 시작한다(스래싱으로 멈춘 VM 포함).
#
# 사용법: reset-run.sh [--no-throttle]
set -euo pipefail
source "$(dirname "$0")/config.sh"

[ -f "$BASE_IMG" ] || { echo "기준 이미지가 없다. build-base.sh를 먼저 실행하라." >&2; exit 1; }

"$VM_DIR/stop.sh" --force >/dev/null 2>&1 || true

rm -f "$RUN_IMG"
"$QEMU_IMG" create -q -f qcow2 -F qcow2 -b "$(winpath "$BASE_IMG")" "$(winpath "$RUN_IMG")"
"$VM_DIR/start.sh" "$RUN_IMG" "$@"

log "SSH 대기"
wait_ssh 300 || { echo "SSH 접속 실패. $IMG_DIR/serial.log 확인" >&2; exit 1; }
log "준비 완료: ssh -i $SSH_KEY -p $SSH_PORT $VM_USER@127.0.0.1"
