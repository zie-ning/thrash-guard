/*
 * policy.c - 종료 안전장치 구현
 *
 * 핵심 경로: 할당 금지.
 */
#include "policy.h"

#define NS_PER_SEC 1000000000ull
#define HOUR_NS (3600ull * NS_PER_SEC)

void tg_policy_init(struct tg_policy *p, uint32_t cooldown_sec, uint32_t max_kills_per_hour)
{
    for (uint32_t i = 0; i < TG_POLICY_RING; i++)
        p->kill_ns[i] = 0;
    p->head = 0;
    p->count = 0;
    p->cooldown_sec = cooldown_sec;
    p->max_kills_per_hour =
        max_kills_per_hour > TG_POLICY_RING ? TG_POLICY_RING : max_kills_per_hour;
}

/* 핵심 경로: 할당 금지 */
enum tg_policy_verdict tg_policy_check(const struct tg_policy *p, uint64_t now_ns)
{
    uint32_t in_hour = 0;

    if (p->count > 0) {
        uint64_t last = p->kill_ns[(p->head + TG_POLICY_RING - 1) % TG_POLICY_RING];

        /* now가 last보다 이르면(시계 이상) 쿨다운 중으로 본다. */
        if (now_ns < last || now_ns - last < (uint64_t)p->cooldown_sec * NS_PER_SEC)
            return TG_POLICY_COOLDOWN;
    }

    for (uint32_t i = 0; i < p->count; i++) {
        uint64_t t = p->kill_ns[i];

        if (t <= now_ns && now_ns - t < HOUR_NS)
            in_hour++;
    }
    if (in_hour >= p->max_kills_per_hour)
        return TG_POLICY_HOURLY_LIMIT;
    return TG_POLICY_OK;
}

void tg_policy_record_kill(struct tg_policy *p, uint64_t now_ns)
{
    p->kill_ns[p->head] = now_ns;
    p->head = (p->head + 1) % TG_POLICY_RING;
    if (p->count < TG_POLICY_RING)
        p->count++;
}

/* 핵심 경로: 할당 금지 */
bool tg_policy_in_grace(uint64_t first_seen_ns, uint64_t now_ns, uint32_t grace_sec)
{
    if (now_ns < first_seen_ns)
        return true;
    return now_ns - first_seen_ns < (uint64_t)grace_sec * NS_PER_SEC;
}

const char *tg_policy_verdict_name(enum tg_policy_verdict v)
{
    switch (v) {
    case TG_POLICY_OK:
        return "ok";
    case TG_POLICY_COOLDOWN:
        return "cooldown";
    case TG_POLICY_HOURLY_LIMIT:
        return "hourly_limit";
    }
    return "unknown";
}
