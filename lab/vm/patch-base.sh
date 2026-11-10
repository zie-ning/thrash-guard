#!/bin/bash
# 기준 이미지를 직접 켜서 VM 안에서 스크립트 하나를 실행하고 끈다.
# provision.sh를 고쳤을 때 전체를 다시 빌드하지 않고 같은 변경을 기준 이미지에 반영하는 용도다.
# 반영한 내용은 반드시 provision.sh에도 넣어, 처음부터 다시 빌드해도 같은 결과가 나오게 한다.
#
# 사용법: patch-base.sh <VM 안에서 root로 실행할 스크립트>
set -euo pipefail
source "$(dirname "$0")/config.sh"

script="${1:?스크립트 경로가 필요하다}"
[ -f "$BASE_IMG" ] || { echo "기준 이미지가 없다" >&2; exit 1; }

"$VM_DIR/stop.sh" --force >/dev/null 2>&1 || true
# 패치 중 실패해도 기준 이미지를 연 VM이 남지 않도록, 정상 종료(ACPI)를 먼저 시도하고 정리한다.
# 정상 경로에서는 이미 꺼져 있으므로 아무 일도 하지 않는다.
trap '"$VM_DIR/stop.sh" >/dev/null 2>&1 || true' EXIT
"$VM_DIR/start.sh" "$BASE_IMG" --no-throttle
wait_ssh 300 || { echo "SSH 접속 실패" >&2; exit 1; }
vm_scp "$script" /tmp/patch.sh
vm_ssh 'sudo bash -euxo pipefail /tmp/patch.sh'
vm_ssh 'sudo systemctl poweroff' || true
wait_qemu_exit 120 || log "정상 종료 시간 초과, 강제 종료함"
log "기준 이미지 패치 완료"
