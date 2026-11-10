#!/bin/bash
# VM을 끈다.
#
# 사용법: stop.sh [--force]
#   기본    : ACPI 종료를 요청하고 최대 60초 기다린 뒤, 그래도 켜져 있으면 강제 종료
#   --force : 바로 QEMU를 종료한다 (스래싱으로 멈춘 VM용, 전원 플러그를 뽑는 것과 같다)
set -euo pipefail
source "$(dirname "$0")/config.sh"

running() { python3 "$VM_DIR/qmp.py" status "$QMP_PORT" >/dev/null 2>&1; }

if ! running; then
    log "실행 중인 VM 없음"
    exit 0
fi

if [ "${1:-}" != "--force" ]; then
    python3 "$VM_DIR/qmp.py" powerdown "$QMP_PORT" >/dev/null || true
    for _ in $(seq 1 30); do
        running || { log "정상 종료"; exit 0; }
        sleep 2
    done
    log "정상 종료 시간 초과, 강제 종료"
fi

python3 "$VM_DIR/qmp.py" quit "$QMP_PORT" >/dev/null 2>&1 || true
for _ in $(seq 1 10); do
    running || { log "강제 종료"; exit 0; }
    sleep 1
done
echo "QEMU가 종료되지 않는다" >&2
exit 1
