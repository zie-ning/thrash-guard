#!/bin/bash
# 실행 메타데이터를 기록한다 (M0-7). 결과를 해석할 때 "어떤 조건에서 쟀는가"를 남기기 위함이다.
#
# 사용법: run_meta.sh <출력 디렉터리>
#   meta.json : 주요 조건 요약 (커널, MGLRU, swap, VM 사양, I/O 상한, git 커밋)
#   env.txt   : VM 안 verify.sh 원본 출력
set -euo pipefail
source "$(dirname "$0")/../vm/config.sh"

out="${1:?출력 디렉터리가 필요하다}"
mkdir -p "$out"

vm_scp "$VM_DIR/verify.sh" /tmp/ >/dev/null
vm_ssh 'sudo bash /tmp/verify.sh' >"$out/env.txt"

# VM 안의 핵심 값은 한 번에 key=value로 받아 JSON으로 만든다.
vm_ssh 'sudo bash -s' >"$out/vm_kv.txt" <<'EOF'
echo "kernel=$(uname -r)"
echo "lru_gen_enabled=$(cat /sys/kernel/mm/lru_gen/enabled 2>/dev/null || echo none)"
echo "lru_gen_min_ttl_ms=$(cat /sys/kernel/mm/lru_gen/min_ttl_ms 2>/dev/null || echo none)"
echo "swappiness=$(sysctl -n vm.swappiness)"
echo "watermark_scale_factor=$(sysctl -n vm.watermark_scale_factor)"
echo "swap_total_kb=$(awk '/^SwapTotal/{print $2}' /proc/meminfo)"
echo "mem_total_kb=$(awk '/^MemTotal/{print $2}' /proc/meminfo)"
echo "oomd=$(systemctl is-active systemd-oomd 2>/dev/null || true)"
EOF

git_sha="$(git -C "$TG_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
git_dirty="$(git -C "$TG_ROOT" status --porcelain 2>/dev/null | grep -q . && echo True || echo False)"

python3 - "$out/vm_kv.txt" "$out/meta.json" <<EOF
import json, sys, datetime
vm = dict(l.rstrip("\n").split("=", 1) for l in open(sys.argv[1]) if "=" in l)
meta = {
    "recorded_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "git": {"sha": "$git_sha", "dirty": $git_dirty},
    "vm": {
        "mem_mb": $VM_MEM_MB, "cpus": $VM_CPUS,
        "io_limit": {"iops": $VM_IOPS, "bytes_per_sec": $VM_BPS},
        "hypervisor": "qemu-whpx",
    },
    "guest": vm,
}
json.dump(meta, open(sys.argv[2], "w"), ensure_ascii=False, indent=2)
EOF
rm -f "$out/vm_kv.txt"
cat "$out/meta.json"
