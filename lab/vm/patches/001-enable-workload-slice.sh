#!/bin/bash
# workload.slice를 부팅 시 자동으로 활성화한다 (provision.sh에 같은 내용 반영됨).
cat >/etc/systemd/system/workload.slice <<'EOF'
[Unit]
Description=Thrash Guard 실험 대상 워크로드
Before=slices.target

[Slice]

[Install]
WantedBy=slices.target
EOF
systemctl daemon-reload
systemctl enable --now workload.slice
ls -d /sys/fs/cgroup/workload.slice
