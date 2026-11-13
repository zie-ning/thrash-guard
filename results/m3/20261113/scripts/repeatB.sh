#!/bin/bash
# 실험 B 나머지 5회: none_1은 이미 끝났다. chain, none을 번갈아 돌린다.
cd "$(dirname "$0")"
for pair in chain_1 none_2 chain_2 none_3 chain_3; do
    arm=${pair%%_*}
    ./m3runB.sh $arm /tmp/m3runB.$pair.log > /tmp/m3runB.$pair.out 2>&1
    cp /tmp/m3runB.$pair.log* .
done
