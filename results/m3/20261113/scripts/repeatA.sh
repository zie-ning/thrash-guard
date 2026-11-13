#!/bin/bash
# 실험 A: 대조군(plain)과 처치군(protected)을 번갈아 3쌍 돌린다.
cd "$(dirname "$0")"
for n in 1 2 3; do
    for v in plain protected; do
        ./m3runA.sh $v /tmp/m3runA.${v}_$n.log > /tmp/m3runA.${v}_$n.out 2>&1
        cp /tmp/m3runA.${v}_$n.log* .
    done
done
