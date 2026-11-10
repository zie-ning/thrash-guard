#!/bin/bash
# VM 환경을 기록한다. VM 안에서 root로 실행한다. 결과는 실행 메타데이터의 기초가 된다.
set -u

section() { printf '\n== %s\n' "$1"; }

section "커널"
uname -r
grep PRETTY_NAME /etc/os-release

section "PSI"
cat /proc/pressure/memory 2>&1

section "cgroup v2"
stat -fc %T /sys/fs/cgroup
cat /sys/fs/cgroup/cgroup.controllers
ls /sys/fs/cgroup/workload.slice/cgroup.kill 2>&1

section "MGLRU"
cat /sys/kernel/mm/lru_gen/enabled 2>&1
cat /sys/kernel/mm/lru_gen/min_ttl_ms 2>&1
grep -E 'CONFIG_LRU_GEN' "/boot/config-$(uname -r)" 2>&1

section "메모리·swap"
free -m
swapon --show
sysctl vm.swappiness vm.watermark_scale_factor vm.min_free_kbytes

section "Docker"
docker --version 2>&1
docker info --format 'cgroup driver={{.CgroupDriver}} version={{.CgroupVersion}}' 2>&1

section "systemd-oomd"
systemctl is-enabled systemd-oomd 2>&1 || true

section "디스크"
lsblk -d -o NAME,SIZE,ROTA,SCHED
df -h /
