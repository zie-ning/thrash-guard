/*
 * judge.c - 스래싱 판단 구현
 *
 * 핵심 경로: 할당 금지. 이 파일의 함수는 힙과 stdio를 쓰지 않는다.
 */
#include "judge.h"

#include <string.h>

/* 관찰 중 시스템 평균이 X의 이 비율 미만이면 관찰을 끝낸다 (근거 없는 임시 상수) */
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

static void memwin_push(struct tg_memwin *w, uint32_t cap, uint64_t kib)
{
    if (w->filled < cap)
        w->filled++;
    w->kib[w->head] = kib;
    w->head = (w->head + 1) % cap;
}

/* 누적값의 증가분. 카운터가 줄었으면 0으로 본다. */
static uint64_t delta(uint64_t now, uint64_t prev)
{
    return now >= prev ? now - prev : 0;
}

/* 윈도와 기준점을 비운다. 관찰 중인지 여부와 건너뜀 기록은 건드리지 않는다. */
static void reset_windows(struct tg_judge *j)
{
    j->sys_has_prev = false;
    j->sys_prev_total_us = 0;
    j->sys_prev_now_ns = 0;
    win_reset(&j->sys_full);
    win_reset(&j->sys_dt);
    for (uint32_t i = 0; i < TG_MAX_TARGETS; i++)
        j->slot[i].used = false;
    j->nstat = 0;
}

static void reset_watch(struct tg_judge *j)
{
    j->watching = false;
    j->last_skip = TG_R_NONE;
    reset_windows(j);
}

void tg_judge_init(struct tg_judge *j, const struct tg_config *cfg)
{
    memset(j, 0, sizeof(*j));
    j->nsamples = tg_config_window_samples(cfg);
    j->span_us = (uint64_t)j->nsamples * cfg->sample_interval_ms * 1000ull;
    j->full_pct = cfg->full_pct;
    j->mem_avail_pct = cfg->mem_avail_pct;
    j->min_size_pct = cfg->min_size_pct;
    j->min_growth_pct = cfg->min_growth_pct;
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
        size_t n;

        if (s->used)
            continue;
        s->used = true;
        memset(&s->mem, 0, sizeof(s->mem));
        n = strnlen(name, sizeof(s->name) - 1);
        memcpy(s->name, name, n);
        s->name[n] = '\0';
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

/* 후보 지표 계산. 윈도가 가득 찼을 때만 최대·증가량을 채운다. */
static void fill_stat(const struct tg_judge *j, const struct tg_judge_slot *sl,
                      const struct tg_cand_sample *c, const struct tg_sample *s,
                      struct tg_cand_stat *st)
{
    uint64_t total_kib = s->mem_total_bytes / 1024;
    uint64_t max_kib = 0;
    uint64_t first_kib;

    memset(st, 0, sizeof(*st));
    st->is_protected = c->is_protected;
    st->in_grace = tg_policy_in_grace(c->first_seen_ns, s->now_ns, j->grace_sec);
    if (sl == NULL || sl->mem.filled < j->nsamples)
        return;
    st->ready = true;

    for (uint32_t i = 0; i < j->nsamples; i++) {
        if (sl->mem.kib[i] > max_kib)
            max_kib = sl->mem.kib[i];
    }
    first_kib = sl->mem.kib[sl->mem.head]; /* 가득 찼으므로 head가 가장 오래된 값 */

    st->max_bytes = max_kib * 1024;
    st->growth_bytes = max_kib > first_kib ? (max_kib - first_kib) * 1024 : 0;
    st->eligible = !st->in_grace && !st->is_protected &&
                   max_kib * 100 >= total_kib * j->min_size_pct && total_kib > 0;
    st->growing = st->growth_bytes / 1024 * 100 >= total_kib * j->min_growth_pct && total_kib > 0;
}

/* a가 b보다 먼저 종료되어야 하면 true. 동률이면 false(앞 번호 유지). */
static bool better(const struct tg_cand_stat *a, const struct tg_cand_stat *b)
{
    if (a->growing != b->growing)
        return a->growing;
    if (a->growing)
        return a->growth_bytes > b->growth_bytes;
    return a->max_bytes > b->max_bytes;
}

/* 핵심 경로: 할당 금지 */
struct tg_decision tg_judge_tick(struct tg_judge *j, const struct tg_sample *s,
                                 const struct tg_policy *pol)
{
    struct tg_decision d;
    uint32_t nc;
    uint32_t sys_avg;
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

    /*
     * 시스템 윈도 갱신. 첫 샘플은 기준점만 잡는다. 직전 샘플과의 간격이 윈도 전체가 대표하는
     * 시간보다 길면(긴 지연, 샘플 읽기 실패 뒤) 윈도의 옛 자료가 최근 10초를 대표하지 못하므로
     * 윈도를 모두 버리고 이번 샘플을 새 기준점으로 삼는다.
     */
    if (j->sys_has_prev) {
        uint64_t dt_us = s->now_ns > j->sys_prev_now_ns ? (s->now_ns - j->sys_prev_now_ns) / 1000 : 0;

        if (dt_us > j->span_us) {
            reset_windows(j);
            j->last_skip = TG_R_NONE;
        }
    }
    if (j->sys_has_prev) {
        uint64_t dt_us = s->now_ns > j->sys_prev_now_ns ? (s->now_ns - j->sys_prev_now_ns) / 1000 : 0;

        win_push(&j->sys_full, j->nsamples, sat32(delta(s->sys_full_total_us, j->sys_prev_total_us)));
        win_push(&j->sys_dt, j->nsamples, sat32(dt_us));
    }
    j->sys_has_prev = true;
    j->sys_prev_total_us = s->sys_full_total_us;
    j->sys_prev_now_ns = s->now_ns;

    /* 이번 샘플에 없는 후보의 상태는 버린다. 다시 나타나면 처음부터 쌓는다. */
    for (uint32_t i = 0; i < TG_MAX_TARGETS; i++) {
        if (j->slot[i].used && !in_sample(s, nc, j->slot[i].name))
            j->slot[i].used = false;
    }

    j->nstat = nc;
    for (uint32_t i = 0; i < nc; i++) {
        const struct tg_cand_sample *c = &s->cand[i];
        struct tg_judge_slot *sl = slot_find(j, c->name);

        if (sl == NULL)
            sl = slot_alloc(j, c->name);
        if (sl != NULL) /* 표 크기가 TG_MAX_TARGETS라 정상 입력에서는 항상 성공한다 */
            memwin_push(&sl->mem, j->nsamples, c->mem_bytes / 1024);
        fill_stat(j, sl, c, s, &j->stat[i]);
    }

    if (j->sys_full.filled < j->nsamples)
        return d; /* 윈도가 다 차기 전에는 결정하지 않는다 */

    sys_avg = avg_x100(j->sys_full.sum, j->sys_dt.sum);
    x100 = j->full_pct * 100;
    d.sys_avg_x100 = sys_avg;

    if (sys_avg < x100 / CALM_DIVISOR) {
        reset_watch(j);
        d.kind = TG_J_IDLE;
        d.reason = TG_R_CALM;
        return d;
    }
    if (sys_avg < x100) {
        j->last_skip = TG_R_NONE; /* 다시 X를 넘으면 사유를 새로 알린다 */
        return d;
    }

    /* 압력이 높다. 여유 메모리가 아직 충분하면 종료하지 않는다. */
    {
        uint64_t total_kib = s->mem_total_bytes / 1024;
        uint64_t avail_kib = s->mem_available_bytes / 1024;

        if (total_kib > 0)
            d.avail_x100 = sat32(avail_kib * 10000 / total_kib);
        if (total_kib == 0 || avail_kib * 100 > total_kib * j->mem_avail_pct) {
            skip_once(j, &d, TG_R_MEM_AVAILABLE);
            return d;
        }
    }

    /* 후보 선택 */
    for (uint32_t i = 0; i < nc; i++) {
        const struct tg_cand_stat *st = &j->stat[i];

        if (st->ready && !st->in_grace && !st->is_protected)
            ready_count++;
        if (!st->eligible)
            continue;
        if (best < 0 || better(st, &j->stat[best]))
            best = (int32_t)i;
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
    d.cand_max_bytes = j->stat[best].max_bytes;
    d.cand_growth_bytes = j->stat[best].growth_bytes;
    d.cand_growing = j->stat[best].growing;
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
    case TG_R_MEM_AVAILABLE:
        return "mem_available";
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
