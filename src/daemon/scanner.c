/*
 * scanner.c - 스캔 스레드 구현
 *
 * 스레드 본문은 할당을 하지 않는다(targets.c가 getdents64와 스택 버퍼만 쓴다).
 */
#include "scanner.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include "mono.h"

/* stop 요청에 빨리 반응하도록 잘게 나눠 잔다. */
static void sleep_ms_until_stop(struct tg_scanner *s, uint32_t ms)
{
    while (ms > 0 && !atomic_load(&s->stop)) {
        uint32_t step = ms > 20 ? 20 : ms;
        struct timespec ts = {0, (long)step * 1000000L};

        nanosleep(&ts, NULL);
        ms -= step;
    }
}

static void *scan_main(void *arg)
{
    struct tg_scanner *s = arg;

    while (!atomic_load(&s->stop)) {
        int cur;
        int nxt;
        int rc;

        sleep_ms_until_stop(s, s->interval_ms);
        if (atomic_load(&s->stop))
            break;

        cur = atomic_load(&s->current);
        nxt = 1 - cur;
        /* 메인이 아직 그 표를 읽고 있으면 이번 회차는 건너뛴다. */
        if (atomic_load(&s->in_use) == nxt)
            continue;

        rc = tg_targets_rebuild(&s->tbl[nxt], &s->tbl[cur], s->parent, tg_now_ns(), false);
        if (rc == 0) {
            atomic_store(&s->current, nxt);
            atomic_fetch_add(&s->scans, 1);
        } else {
            atomic_fetch_add(&s->scan_errors, 1);
        }
    }
    return NULL;
}

int tg_scanner_start(struct tg_scanner *s, const char *parent, uint32_t interval_ms)
{
    pthread_attr_t attr;
    size_t plen = strlen(parent);
    int rc;

    s->started = false; /* 시작에 실패한 뒤 tg_scanner_stop을 불러도 안전하도록 가장 먼저 */
    if (plen >= sizeof(s->parent) || interval_ms == 0)
        return -EINVAL;

    memset(&s->thread, 0, sizeof(s->thread));
    tg_targets_init(&s->tbl[0]);
    tg_targets_init(&s->tbl[1]);
    atomic_init(&s->current, 0);
    atomic_init(&s->in_use, -1);
    atomic_init(&s->stop, false);
    atomic_init(&s->scans, 0);
    atomic_init(&s->scan_errors, 0);
    memcpy(s->parent, parent, plen + 1);
    s->interval_ms = interval_ms;

    rc = tg_targets_rebuild(&s->tbl[0], NULL, s->parent, tg_now_ns(), true);
    if (rc != 0)
        return rc;

    rc = pthread_attr_init(&attr);
    if (rc == 0) {
        rc = pthread_attr_setstacksize(&attr, TG_SCANNER_STACK_BYTES);
        if (rc == 0)
            rc = pthread_create(&s->thread, &attr, scan_main, s);
        pthread_attr_destroy(&attr);
    }
    if (rc != 0) {
        tg_targets_release(&s->tbl[0], NULL);
        return -rc;
    }
    s->started = true;
    return 0;
}

const struct tg_targets *tg_scanner_acquire(struct tg_scanner *s)
{
    for (;;) {
        int idx = atomic_load(&s->current);

        atomic_store(&s->in_use, idx);
        /* 표를 잡는 사이에 교체됐으면 다시 시도한다. */
        if (atomic_load(&s->current) == idx)
            return &s->tbl[idx];
    }
}

void tg_scanner_release(struct tg_scanner *s)
{
    atomic_store(&s->in_use, -1);
}

void tg_scanner_stop(struct tg_scanner *s)
{
    int cur;

    if (!s->started)
        return;
    atomic_store(&s->stop, true);
    pthread_join(s->thread, NULL);
    s->started = false;

    /* 두 표는 fd를 공유하므로 공개 표에 없는 것부터 닫고 나머지를 닫는다. */
    cur = atomic_load(&s->current);
    tg_targets_release(&s->tbl[1 - cur], &s->tbl[cur]);
    tg_targets_release(&s->tbl[cur], NULL);
}
