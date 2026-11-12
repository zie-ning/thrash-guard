/*
 * mono.h - 단조 시계
 *
 * 모든 시각 비교(유예, 쿨다운, 시간당 상한)는 CLOCK_MONOTONIC 나노초를 쓴다.
 * 시스템 시각이 바뀌어도 영향받지 않는다.
 */
#ifndef TG_MONO_H
#define TG_MONO_H

#include <stdint.h>
#include <time.h>

static inline uint64_t tg_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif /* TG_MONO_H */
