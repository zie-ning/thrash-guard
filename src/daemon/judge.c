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

/* 가장 오래된 칸을 뺀다. */
static void win_pop_oldest(struct tg_win *w, uint32_t cap)
{
    uint32_t idx = (w->head + cap - w->filled) % cap;

    w->sum -= w->v[idx];
    w->filled--;
}

/*
 * 가장 오래된 칸을 빼도 윈도가 대표하는 시간(span)을 채우면 뺀다. 공백(지연, 읽기 실패)이 있었던
 * 뒤에는 칸 수는 적어도 시간으로는 옛 자료가 최근 span을 대표하지 못하기 때문이다.
 * 공백 하나가 경계에 걸치면 그 칸은 통째로 남는다(칸 안의 시간 분포를 알 수 없다).
 */
static void sys_trim(struct tg_judge *j)
{
    uint32_t cap = j->nsamples;

    while (j->sys_dt.filled > 1) {
        uint32_t idx = (j->sys_dt.head + cap - j->sys_dt.filled) % cap;

        if (j->sys_dt.sum - j->sys_dt.v[idx] < j->span_us)
            break;
        win_pop_oldest(&j->sys_full, cap);
        win_pop_oldest(&j->sys_dt, cap);
    }
}

static uint32_t memwin_oldest(const struct tg_memwin *w, uint32_t cap)
{
    return (w->head + cap - w->filled) % cap;
}

static void memwin_push(struct tg_judge *j, struct tg_memwin *w, uint64_t kib, uint64_t now_ns)
{
    uint32_t cap = j->nsamples;
    uint64_t span_ns = j->span_us * 1000ull;

    if (w->filled < cap)
        w->filled++;
    w->kib[w->head] = kib;
    w->ns[w->head] = now_ns;
    w->head = (w->head + 1) % cap;

    while (w->filled > 1) {
        uint32_t second = (memwin_oldest(w, cap) + 1) % cap;
        uint64_t covered = now_ns > w->ns[second] ? now_ns - w->ns[second] : 0;

        if (covered + j->interval_ns < span_ns)
            break;
        w->filled--;
    }
}

/*
 * 결정에 쓸 시스템 윈도의 합. 가장 오래된 칸이 윈도 경계에 걸쳐 있으면(그 칸의 앞부분이
 * 최근 span 밖) 칸 안의 stall이 윈도 밖에서 먼저 일어났다고 가정해 윈도 안 몫을 가장 적게 잡는다.
 * 칸 안의 시간 분포를 알 수 없으므로 과대 추정으로 잘못 종료하는 것보다 놓치는 쪽을 택한다.
 * 윈도가 span보다 길지 않으면(정상적인 주기) 칸을 통째로 쓴다.
 */
static void sys_totals(const struct tg_judge *j, uint64_t *full_us, uint64_t *dt_us)
{
    uint32_t cap = j->nsamples;
    uint64_t sum_full = j->sys_full.sum;
    uint64_t sum_dt = j->sys_dt.sum;

    *full_us = sum_full;
    *dt_us = sum_dt;
    if (sum_dt > j->span_us && j->sys_dt.filled > 1) {
        uint32_t idx = (j->sys_dt.head + cap - j->sys_dt.filled) % cap;
        uint64_t ofull = j->sys_full.v[idx];
        uint64_t odt = j->sys_dt.v[idx];
        uint64_t rest_dt = sum_dt - odt;
        uint64_t inwin = j->span_us > rest_dt ? j->span_us - rest_dt : 0;
        uint64_t outside = odt > inwin ? odt - inwin : 0;
        uint64_t in_full = ofull > outside ? ofull - outside : 0;

        *full_us = sum_full - ofull + in_full;
        *dt_us = rest_dt + inwin;
    }
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
    j->interval_ns = (uint64_t)cfg->sample_interval_ms * 1000000ull;
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

static struct tg_judge_slot *slot_alloc(struct tg_judge *j, const char *name, uint64_t first_seen_ns)
{
    for (uint32_t i = 0; i < TG_MAX_TARGETS; i++) {
        struct tg_judge_slot *s = &j->slot[i];
        size_t n;

        if (s->used)
            continue;
        s->used = true;
        s->first_seen_ns = first_seen_ns;
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
    if (sl == NULL || sl->mem.filled == 0)
        return;

    /*
     * 최근 span 안의 샘플만 쓴다. 공백 뒤에는 옛 최대값이 span보다 오래 남아 있을 수 있는데,
     * 그 값으로 자격이나 증가량을 판단하지 않는다. 남은 샘플이 span을 덮지 못하면 아직 준비되지 않은 것이다.
     */
    {
        uint32_t cap = j->nsamples;
        uint64_t span_ns = j->span_us * 1000ull;
        uint32_t oldest = memwin_oldest(&sl->mem, cap);
        uint32_t first_in = sl->mem.filled;
        uint64_t newest_ns = sl->mem.ns[(sl->mem.head + cap - 1) % cap];

        for (uint32_t k = 0; k < sl->mem.filled; k++) {
            uint64_t t = sl->mem.ns[(oldest + k) % cap];

            if (s->now_ns <= t || s->now_ns - t <= span_ns) {
                first_in = k;
                break;
            }
        }
        if (first_in == sl->mem.filled)
            return;
        if ((newest_ns - sl->mem.ns[(oldest + first_in) % cap]) + j->interval_ns < span_ns)
            return;
        st->ready = true;

        for (uint32_t k = first_in; k < sl->mem.filled; k++) {
            uint64_t v = sl->mem.kib[(oldest + k) % cap];

            if (v > max_kib)
                max_kib = v;
        }
        first_kib = sl->mem.kib[(oldest + first_in) % cap];
    }

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
        sys_trim(j);
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

        if (sl != NULL && sl->first_seen_ns != c->first_seen_ns) {
            /* 같은 이름이지만 다시 만들어진 cgroup이다. 옛 사용량 기록을 버린다 */
            sl->used = false;
            sl = NULL;
        }
        if (sl == NULL)
            sl = slot_alloc(j, c->name, c->first_seen_ns);
        if (sl != NULL) /* 표 크기가 TG_MAX_TARGETS라 정상 입력에서는 항상 성공한다 */
            memwin_push(j, &sl->mem, c->mem_bytes / 1024, s->now_ns);
        fill_stat(j, sl, c, s, &j->stat[i]);
    }

    if (j->sys_dt.sum < j->span_us)
        return d; /* 윈도가 대표하는 시간을 채우기 전에는 결정하지 않는다 */

    {
        uint64_t full_us;
        uint64_t dt_us;

        sys_totals(j, &full_us, &dt_us);
        sys_avg = avg_x100(full_us, dt_us);
    }
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

    /* 보호 상태가 낡았으면 종료하지 않는다. 그동안 보호 설정이 바뀌었을 수 있다. */
    if (s->targets_stale) {
        skip_once(j, &d, TG_R_STALE_TARGETS);
        return d;
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
    case TG_R_STALE_TARGETS:
        return "targets_stale";
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
