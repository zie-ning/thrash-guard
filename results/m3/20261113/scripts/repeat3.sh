#!/bin/bash
# full_pct=10 기본값으로 실제 종료 모드를 3회 반복한다.
cd "$(dirname "$0")"
for n in 1 2 3; do
    ./m3run3.sh kill /tmp/m3run.k10_$n.log > /tmp/m3run.k10_$n.out 2>&1
    cp /tmp/m3run.k10_$n.log* .
done
