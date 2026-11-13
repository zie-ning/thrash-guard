#!/bin/bash
# 새 판단 규칙(io 경로) VM 확인: 처리량 제한 조건에서 dry-run 2회, 실제 종료 2회.
cd "$(dirname "$0")"
for mode in dry kill dry kill; do
    n=$(ls /tmp/m3runD.${mode}_*.log 2>/dev/null | wc -l); n=$((n+1))
    ./m3runC.sh $mode /tmp/m3runD.${mode}_$n.log > /tmp/m3runD.${mode}_$n.out 2>&1
    cp /tmp/m3runD.${mode}_$n.log* .
done
