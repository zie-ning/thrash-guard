/*
 * engine.c - 트리거와 틱 처리 구현
 *
 * 핵심 경로: 할당 금지. 이 파일의 함수는 힙과 stdio를 쓰지 않는다(로그는 링버퍼 호출).
 */
#include "engine.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "killer.h"

#define LATE_FACTOR 2 /* 예정보다 틱 간격의 이 배수 이상 늦으면 경고한다 (임시 기준) */

int tg_engine_init(struct tg_engine *e, const struct tg_config *cfg, struct tg_log *log,
                   struct tg_scanner *scanner, const char *psi_path, const char *meminfo_path)
{
    int rc;

    memset(e, 0, sizeof(*e));
    e->cfg = cfg;
    e->log = log;
    e->scanner = scanner;
    e->parent_fd = -1;
    e->reader.psi_fd = -1;
    e->reader.meminfo_fd = -1;

    e->parent_fd = open(cfg->target_parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (e->parent_fd < 0)
        return -errno;
    rc = tg_reader_open(&e->reader, psi_path, meminfo_path);
    if (rc != 0) {
        close(e->parent_fd);
        e->parent_fd = -1;
        return rc;
    }

    tg_judge_init(&e->judge, cfg);
    tg_policy_init(&e->policy, cfg->cooldown_sec, cfg->max_kills_per_hour);
    e->interval_ns = (uint64_t)cfg->sample_interval_ms * 1000000ull;
    return 0;
}

void tg_engine_close(struct tg_engine *e)
{
    tg_reader_close(&e->reader);
    if (e->parent_fd >= 0)
        close(e->parent_fd);
    e->parent_fd = -1;
}

/* ---------- 로그 ---------- */

static bool log_start(struct tg_engine *e, enum tg_log_level lv, struct tg_buf *b)
{
    return e->log != NULL && tg_log_begin(e->log, lv, b);
}

static void log_event(struct tg_engine *e, enum tg_log_level lv, const char *what)
{
    struct tg_buf b;

    if (!log_start(e, lv, &b))
        return;
    tg_buf_str(&b, what);
    tg_log_end(e->log, &b);
}

static void log_skip(struct tg_engine *e, const struct tg_decision *d)
{
    struct tg_buf b;

    if (!log_start(e, TG_LOG_INFO, &b))
        return;
    tg_buf_str(&b, "judge skip");
    tg_buf_kv_str(&b, "reason", tg_judge_reason_name(d->reason));
    tg_buf_kv_u64(&b, "sys_avg_x100", d->sys_avg_x100);
    tg_buf_kv_u64(&b, "avail_x100", d->avail_x100);
    tg_log_end(e->log, &b);
}

static void log_calm(struct tg_engine *e, const struct tg_decision *d)
{
    struct tg_buf b;

    if (!log_start(e, TG_LOG_INFO, &b))
        return;
    tg_buf_str(&b, "judge calm");
    tg_buf_kv_u64(&b, "sys_avg_x100", d->sys_avg_x100);
    tg_log_end(e->log, &b);
}

static void log_kill(struct tg_engine *e, const struct tg_decision *d, const char *name, int rc,
                     bool dry)
{
    struct tg_buf b;

    if (!log_start(e, rc == 0 ? TG_LOG_WARN : TG_LOG_ERR, &b))
        return;
    tg_buf_str(&b, "judge kill");
    tg_buf_kv_str(&b, "cand", name);
    tg_buf_kv_u64(&b, "dry_run", dry ? 1 : 0);
    tg_buf_kv_i64(&b, "result", rc);
    tg_buf_kv_u64(&b, "sys_avg_x100", d->sys_avg_x100);
    tg_buf_kv_u64(&b, "avail_x100", d->avail_x100);
    tg_buf_kv_u64(&b, "max_bytes", d->cand_max_bytes);
    tg_buf_kv_u64(&b, "growth_bytes", d->cand_growth_bytes);
    tg_buf_kv_u64(&b, "growing", d->cand_growing ? 1 : 0);
    tg_log_end(e->log, &b);
}

/* 종료 결정 직후 후보별 지표를 남겨 선택 규칙을 사후에 점검할 수 있게 한다. */
static void log_candidates(struct tg_engine *e, const struct tg_sample_buf *sb)
{
    uint32_t n = 0;
    const struct tg_cand_stat *st = tg_judge_stats(&e->judge, &n);

    for (uint32_t i = 0; i < n && i < sb->s.ncand; i++) {
        struct tg_buf b;

        if (!log_start(e, TG_LOG_INFO, &b))
            return;
        tg_buf_str(&b, "judge cand");
        tg_buf_kv_str(&b, "name", sb->cand[i].name);
        tg_buf_kv_u64(&b, "max_bytes", st[i].max_bytes);
        tg_buf_kv_u64(&b, "growth_bytes", st[i].growth_bytes);
        tg_buf_kv_u64(&b, "ready", st[i].ready ? 1 : 0);
        tg_buf_kv_u64(&b, "eligible", st[i].eligible ? 1 : 0);
        tg_buf_kv_u64(&b, "growing", st[i].growing ? 1 : 0);
        tg_buf_kv_u64(&b, "grace", st[i].in_grace ? 1 : 0);
        tg_buf_kv_u64(&b, "protected", st[i].is_protected ? 1 : 0);
        tg_log_end(e->log, &b);
    }
}

/* ---------- 결정 처리 ---------- */

/* 종료(또는 dry-run의 가상 종료)를 처리한다. 정책 기록은 성공한 경우에만 남긴다. */
static void do_kill(struct tg_engine *e, const struct tg_decision *d, uint64_t now_ns)
{
    const char *name = e->sbuf.cand[d->cand_index].name;
    bool dry = e->cfg->dry_run;
    int rc = 0;

    if (dry) {
        e->st.dry_kills++;
    } else {
        rc = tg_kill_cgroup(e->parent_fd, name);
        if (rc == 0)
            e->st.kills++;
        else
            e->st.kill_errors++;
    }
    log_kill(e, d, name, rc, dry);
    if (rc == 0)
        tg_policy_record_kill(&e->policy, now_ns);
    log_candidates(e, &e->sbuf);
}

void tg_engine_trigger(struct tg_engine *e, uint64_t now_ns)
{
    if (e->watching)
        return;
    e->st.triggers++;
    tg_judge_trigger(&e->judge);
    e->watching = true;
    e->delay_warned = false;
    e->error_warned = false;
    e->skipped_warned = false;
    e->deadline_ns = now_ns; /* 첫 샘플을 바로 찍어 PSI 기준점을 잡는다 */
    log_event(e, TG_LOG_INFO, "trigger watch start");
}

static void warn_late(struct tg_engine *e, uint64_t delay_ns)
{
    struct tg_buf b;

    if (!log_start(e, TG_LOG_WARN, &b))
        return;
    tg_buf_str(&b, "loop late");
    tg_buf_kv_u64(&b, "delay_ms", delay_ns / 1000000ull);
    tg_buf_kv_u64(&b, "interval_ms", e->interval_ns / 1000000ull);
    tg_log_end(e->log, &b);
}

static void warn_sample_error(struct tg_engine *e, int rc)
{
    struct tg_buf b;

    if (!log_start(e, TG_LOG_WARN, &b))
        return;
    tg_buf_str(&b, "sample read failed");
    tg_buf_kv_i64(&b, "errno", -rc);
    tg_log_end(e->log, &b);
}

/* 핵심 경로: 할당 금지 */
void tg_engine_tick(struct tg_engine *e, uint64_t now_ns)
{
    const struct tg_targets *t;
    struct tg_decision d;
    uint64_t delay = now_ns > e->deadline_ns ? now_ns - e->deadline_ns : 0;
    int rc;

    if (!e->watching)
        return;
    e->st.ticks++;

    if (delay > e->st.max_delay_ns)
        e->st.max_delay_ns = delay;
    if (delay >= LATE_FACTOR * e->interval_ns) {
        e->st.late_ticks++;
        if (!e->delay_warned) {
            e->delay_warned = true;
            warn_late(e, delay);
        }
    }

    t = tg_scanner_acquire(e->scanner);
    if (t->skipped > 0 && !e->skipped_warned) {
        struct tg_buf b;

        e->skipped_warned = true;
        if (log_start(e, TG_LOG_WARN, &b)) {
            tg_buf_str(&b, "targets full");
            tg_buf_kv_u64(&b, "max", TG_MAX_TARGETS);
            tg_buf_kv_u64(&b, "skipped", t->skipped);
            tg_log_end(e->log, &b);
        }
    }
    rc = tg_sample_collect(&e->reader, t, now_ns, &e->sbuf);
    if (rc != 0) {
        e->st.sample_errors++;
        if (!e->error_warned) {
            e->error_warned = true;
            warn_sample_error(e, rc);
        }
    } else {
        d = tg_judge_tick(&e->judge, &e->sbuf.s, &e->policy);
        switch (d.kind) {
        case TG_J_WATCHING:
            break;
        case TG_J_SKIP:
            e->st.skips++;
            log_skip(e, &d);
            break;
        case TG_J_KILL:
            do_kill(e, &d, now_ns);
            e->watching = false;
            break;
        case TG_J_IDLE:
            if (d.reason == TG_R_CALM) {
                e->st.calms++;
                log_calm(e, &d);
            }
            e->watching = false;
            break;
        }
    }
    tg_scanner_release(e->scanner);

    if (e->watching) {
        uint64_t next = e->deadline_ns + e->interval_ns;

        /* 많이 밀렸으면 따라잡으려고 연달아 읽지 않는다. 윈도 칸이 시간을 대표하지 못하게 되기 때문이다. */
        if (next <= now_ns)
            next = now_ns + e->interval_ns;
        e->deadline_ns = next;
    }
}

void tg_engine_log_summary(struct tg_engine *e)
{
    struct tg_buf b;

    if (!log_start(e, TG_LOG_INFO, &b))
        return;
    tg_buf_str(&b, "stats");
    tg_buf_kv_u64(&b, "triggers", e->st.triggers);
    tg_buf_kv_u64(&b, "ticks", e->st.ticks);
    tg_buf_kv_u64(&b, "skips", e->st.skips);
    tg_buf_kv_u64(&b, "calms", e->st.calms);
    tg_buf_kv_u64(&b, "kills", e->st.kills);
    tg_buf_kv_u64(&b, "dry_kills", e->st.dry_kills);
    tg_buf_kv_u64(&b, "kill_errors", e->st.kill_errors);
    tg_buf_kv_u64(&b, "sample_errors", e->st.sample_errors);
    tg_buf_kv_u64(&b, "late_ticks", e->st.late_ticks);
    tg_buf_kv_u64(&b, "max_delay_ms", e->st.max_delay_ns / 1000000ull);
    tg_log_end(e->log, &b);
}
