/*
 * judge.c - 스래싱 판단 구현
 *
 * 핵심 경로: 할당 금지. 이 파일의 함수는 힙과 stdio를 쓰지 않는다.
 */
#include "judge.h"

#include <string.h>

/* 관찰 중 구역 평균이 X의 이 비율 미만이면 관찰을 끝낸다 (D22, 근거 없는 임시 상수) */
#define CALM_DIVISOR 2

static uint32_t sat32(uint64_t v)
{
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

static void win_reset(struct tg_win *w)
{
    memset(w, 0, sizeof(*w));
}

static void win_push(struct tg_win *w, uint32_t cap, uint32_t v)
{
    if (w->filled == cap)
        w->sum -= w->v[w->head];
    else
        w->filled++;
    w->v[w->head] = v;
    w->sum += v;
    w->head = (w->head + 1) % cap;
}

/* 누적값의 증가분. 카운터가 줄었으면(cgroup 재생성 등) 0으로 본다. */
static uint64_t delta(uint64_t now, uint64_t prev)
{
    return now >= prev ? now - prev : 0;
}

static void reset_watch(struct tg_judge *j)
{
    j->watching = false;
    j->last_skip = TG_R_NONE;
    j->zone_has_prev = false;
    j->zone_prev_total_us = 0;
    j->zone_prev_now_ns = 0;
    win_reset(&j->zone_full);
    win_reset(&j->zone_dt);
    for (uint32_t i = 0; i < TG_MAX_TARGETS; i++)
        j->slot[i].used = false;
    j->nstat = 0;
}

void tg_judge_init(struct tg_judge *j, const struct tg_config *cfg)
{
    memset(j, 0, sizeof(*j));
    j->nsamples = tg_config_window_samples(cfg);
    j->full_pct = cfg->full_pct;
    j->grace_sec = cfg->startup_grace_sec;
    reset_watch(j);
}

void tg_judge_trigger(struct tg_judge *j)
{
    if (j->watching)
        return;
    reset_watch(j);
    j->watching = true;
}

static struct tg_judge_slot *slot_find(struct tg_judge *j, const char *name)
{
    for (uint32_t i = 0; i < TG_MAX_TARGETS; i++) {
        if (j->slot[i].used && strcmp(j->slot[i].name, name) == 0)
            return &j->slot[i];
    }
    return NULL;
}

static struct tg_judge_slot *slot_alloc(struct tg_judge *j, const char *name)
{
    for (uint32_t i = 0; i < TG_MAX_TARGETS; i++) {
        struct tg_judge_slot *s = &j->slot[i];

        if (s->used)
            continue;
        s->used = true;
        s->has_prev = false;
        s->prev_full_us = 0;
        s->prev_pgscan = 0;
        win_reset(&s->full);
        win_reset(&s->scan);
        {
            size_t n = strnlen(name, sizeof(s->name) - 1);

            memcpy(s->name, name, n);
            s->name[n] = '\0';
        }
        return s;
    }
    return NULL;
}

static bool in_sample(const struct tg_sample *s, uint32_t nc, const char *name)
{
    for (uint32_t i = 0; i < nc; i++) {
        if (strcmp(s->cand[i].name, name) == 0)
            return true;
    }
    return false;
}

static uint32_t avg_x100(uint64_t full_us, uint64_t dt_us)
{
    if (dt_us == 0)
        return 0;
    return sat32(full_us * 10000ull / dt_us);
}

/* 같은 사유는 관찰 한 번에 한 번만 알린다. */
static void skip_once(struct tg_judge *j, struct tg_decision *d, enum tg_judge_reason r)
{
    if (j->last_skip == r) {
        d->kind = TG_J_WATCHING;
        d->reason = TG_R_NONE;
        return;
    }
    j->last_skip = r;
    d->kind = TG_J_SKIP;
    d->reason = r;
}

/* 핵심 경로: 할당 금지 */
struct tg_decision tg_judge_tick(struct tg_judge *j, const struct tg_sample *s,
                                 const struct tg_policy *pol)
{
    struct tg_decision d;
    struct tg_judge_slot *slot_of[TG_MAX_TARGETS];
    uint32_t nc;
    uint32_t zone_avg;
    uint32_t x100;
    int32_t best = -1;
    uint32_t ready_count = 0;

    memset(&d, 0, sizeof(d));
    d.cand_index = -1;
    d.kind = TG_J_IDLE;
    j->nstat = 0;
    if (!j->watching)
        return d;
    d.kind = TG_J_WATCHING;

    nc = s->ncand > TG_MAX_TARGETS ? TG_MAX_TARGETS : s->ncand;

    /* 구역 윈도 갱신. 첫 샘플은 기준점만 잡는다. */
    if (j->zone_has_prev) {
        uint64_t dt_us = s->now_ns > j->zone_prev_now_ns ? (s->now_ns - j->zone_prev_now_ns) / 1000 : 0;

        win_push(&j->zone_full, j->nsamples, sat32(delta(s->zone_full_total_us, j->zone_prev_total_us)));
        win_push(&j->zone_dt, j->nsamples, sat32(dt_us));
    }
    j->zone_has_prev = true;
    j->zone_prev_total_us = s->zone_full_total_us;
    j->zone_prev_now_ns = s->now_ns;

    /* 이번 샘플에 없는 후보의 상태는 버린다. 다시 나타나면 처음부터 쌓는다. */
    for (uint32_t i = 0; i < TG_MAX_TARGETS; i++) {
        if (j->slot[i].used && !in_sample(s, nc, j->slot[i].name))
            j->slot[i].used = false;
    }

    for (uint32_t i = 0; i < nc; i++) {
        const struct tg_cand_sample *c = &s->cand[i];
        struct tg_judge_slot *sl = slot_find(j, c->name);

        if (sl == NULL)
            sl = slot_alloc(j, c->name);
        slot_of[i] = sl;
        if (sl == NULL)
            continue; /* 표 크기가 TG_MAX_TARGETS라 정상 입력에서는 일어나지 않는다 */
        if (sl->has_prev) {
            win_push(&sl->full, j->nsamples, sat32(delta(c->full_total_us, sl->prev_full_us)));
            win_push(&sl->scan, j->nsamples, sat32(delta(c->pgscan, sl->prev_pgscan)));
        }
        sl->has_prev = true;
        sl->prev_full_us = c->full_total_us;
        sl->prev_pgscan = c->pgscan;
    }

    if (j->zone_full.filled < j->nsamples) {
        zone_avg = 0;
    } else {
        zone_avg = avg_x100(j->zone_full.sum, j->zone_dt.sum);
    }

    /* 후보별 지표 */
    x100 = j->full_pct * 100;
    j->nstat = nc;
    for (uint32_t i = 0; i < nc; i++) {
        struct tg_cand_stat *st = &j->stat[i];
        const struct tg_judge_slot *sl = slot_of[i];

        memset(st, 0, sizeof(*st));
        st->mem_bytes = s->cand[i].mem_bytes;
        st->in_grace = tg_policy_in_grace(s->cand[i].first_seen_ns, s->now_ns, j->grace_sec);
        if (sl == NULL)
            continue;
        st->pgscan_delta = sl->scan.sum;
        st->ready = sl->full.filled == j->nsamples && j->zone_full.filled == j->nsamples;
        if (st->ready) {
            st->full_avg_x100 = avg_x100(sl->full.sum, j->zone_dt.sum);
            st->eligible = !st->in_grace && st->full_avg_x100 >= x100;
        }
    }

    d.zone_avg_x100 = zone_avg;
    if (j->zone_full.filled < j->nsamples)
        return d; /* 윈도가 다 차기 전에는 결정하지 않는다 */

    if (zone_avg < x100 / CALM_DIVISOR) {
        reset_watch(j);
        d.kind = TG_J_IDLE;
        d.reason = TG_R_CALM;
        return d;
    }
    if (zone_avg < x100) {
        j->last_skip = TG_R_NONE; /* 다시 X를 넘으면 사유를 새로 알린다 */
        return d;
    }

    /* 평균 >= X: 후보 선택 */
    for (uint32_t i = 0; i < nc; i++) {
        const struct tg_cand_stat *st = &j->stat[i];

        if (st->ready && !st->in_grace)
            ready_count++;
        if (!st->eligible)
            continue;
        if (best < 0) {
            best = (int32_t)i;
            continue;
        }
        {
            const struct tg_cand_stat *b = &j->stat[best];

            if (st->pgscan_delta > b->pgscan_delta ||
                (st->pgscan_delta == b->pgscan_delta && st->mem_bytes > b->mem_bytes))
                best = (int32_t)i;
        }
    }

    if (best < 0) {
        skip_once(j, &d, ready_count > 0 ? TG_R_NO_CULPRIT : TG_R_NO_CANDIDATE);
        return d;
    }

    switch (tg_policy_check(pol, s->now_ns)) {
    case TG_POLICY_COOLDOWN:
        skip_once(j, &d, TG_R_COOLDOWN);
        return d;
    case TG_POLICY_HOURLY_LIMIT:
        skip_once(j, &d, TG_R_HOURLY_LIMIT);
        return d;
    case TG_POLICY_OK:
        break;
    }

    d.kind = TG_J_KILL;
    d.reason = TG_R_NONE;
    d.cand_index = best;
    d.cand_avg_x100 = j->stat[best].full_avg_x100;
    d.cand_pgscan_delta = j->stat[best].pgscan_delta;
    d.cand_mem_bytes = j->stat[best].mem_bytes;
    j->watching = false; /* 윈도는 stats 조회를 위해 남기고, 다음 trigger에서 비운다 */
    j->last_skip = TG_R_NONE;
    return d;
}

const struct tg_cand_stat *tg_judge_stats(const struct tg_judge *j, uint32_t *n)
{
    *n = j->nstat;
    return j->stat;
}

const char *tg_judge_reason_name(enum tg_judge_reason r)
{
    switch (r) {
    case TG_R_NONE:
        return "none";
    case TG_R_CALM:
        return "calm";
    case TG_R_NO_CANDIDATE:
        return "no_candidate";
    case TG_R_NO_CULPRIT:
        return "no_culprit";
    case TG_R_COOLDOWN:
        return "cooldown";
    case TG_R_HOURLY_LIMIT:
        return "hourly_limit";
    }
    return "unknown";
}

const char *tg_judge_kind_name(enum tg_judge_kind k)
{
    switch (k) {
    case TG_J_IDLE:
        return "idle";
    case TG_J_WATCHING:
        return "watching";
    case TG_J_SKIP:
        return "skip";
    case TG_J_KILL:
        return "kill";
    }
    return "unknown";
}
