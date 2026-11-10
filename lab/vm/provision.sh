#!/bin/bash
# 기준 이미지 프로비저닝. VM 안에서 root로 한 번 실행한다 (build-base.sh가 호출).
set -euxo pipefail
export DEBIAN_FRONTEND=noninteractive

# 실험 중 예고 없이 디스크·메모리를 쓰는 자동 업데이트를 끈다. 실행마다 조건이 달라지는 것을 막는다.
systemctl disable --now apt-daily.timer apt-daily-upgrade.timer || true
systemctl disable --now unattended-upgrades.service || true
# 타이머를 꺼도 이미 돌고 있는 apt 서비스가 패키지 목록 잠금을 쥐고 있을 수 있다. 끝날 때까지 기다린다.
for _ in $(seq 1 120); do
    systemctl is-active --quiet apt-daily.service apt-daily-upgrade.service || break
    sleep 5
done

apt-get update
# 6.8 계열 HWE 커널. VM용 virtual 변형은 linux-firmware 등 실험과 무관한 큰 패키지를 끌어오지 않는다.
apt-get install -y --no-install-recommends linux-image-virtual-hwe-22.04 docker.io stress-ng
apt-get clean

# 실험 대상 컨테이너만 들어갈 전용 slice. 데몬과 systemd-oomd 모두 이 아래만 대상으로 삼는다.
cat >/etc/systemd/system/workload.slice <<'EOF'
[Unit]
Description=Thrash Guard 실험 대상 워크로드
Before=slices.target

[Slice]

[Install]
WantedBy=slices.target
EOF
systemctl daemon-reload
# 부팅마다 cgroup 디렉터리가 미리 만들어지도록 enable한다. 데몬은 시작할 때 이 경로를 찾는다.
systemctl enable --now workload.slice

usermod -aG docker tg
