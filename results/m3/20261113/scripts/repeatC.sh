#!/bin/bash
# 실험 C: 실험 B가 끝나기를 기다렸다가 dry, kill을 번갈아 3쌍 돌린다.
cd "$(dirname "$0")"
while pgrep -f "runB.sh" >/dev/null; do sleep 15; done
for n in 1 2 3; do
    for mode in dry kill; do
        ./m3runC.sh $mode /tmp/m3runC.${mode}_$n.log > /tmp/m3runC.${mode}_$n.out 2>&1
        cp /tmp/m3runC.${mode}_$n.log* .
    done
done
