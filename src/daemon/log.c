/*
 * log.c - 로그 링버퍼와 기록 스레드 구현
 *
 * 생산자(메인 루프)가 부르는 tg_log_begin/tg_log_end는 핵심 경로라 할당과 stdio를 쓰지 않는다.
 * 기록 스레드도 같은 이유로 stdio를 쓰지 않고 write(2)만 쓴다.
 */
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mono.h"

#define NS_PER_SEC 1000000000ull

/* ---------- 시각 문자열 ---------- */

static void put_num(char *p, uint32_t v, int width)
{
    for (int i = width - 1; i >= 0; i--) {
        p[i] = (char)('0' + v % 10);
        v /= 10;
    }
}

size_t tg_log_fmt_time(char *out, uint64_t unix_ns)
{
    uint64_t secs = unix_ns / NS_PER_SEC;
    uint32_t ms = (uint32_t)(unix_ns % NS_PER_SEC / 1000000ull);
    int64_t days = (int64_t)(secs / 86400);
    uint32_t rem = (uint32_t)(secs % 86400);
    /* 일 수 -> 연월일 (Howard Hinnant의 civil_from_days) */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;

    if (m <= 2)
        y++;
    put_num(out, (uint32_t)y, 4);
    out[4] = '-';
    put_num(out + 5, m, 2);
    out[7] = '-';
    put_num(out + 8, d, 2);
    out[10] = 'T';
    put_num(out + 11, rem / 3600, 2);
    out[13] = ':';
    put_num(out + 14, rem % 3600 / 60, 2);
    out[16] = ':';
    put_num(out + 17, rem % 60, 2);
    out[19] = '.';
    put_num(out + 20, ms, 3);
    out[23] = 'Z';
    out[24] = '\0';
    return TG_LOG_TS_LEN;
}

/* ---------- 생산자 ---------- */

/* 핵심 경로: 할당 금지 */
bool tg_log_begin(struct tg_log *l, enum tg_log_level lv, struct tg_buf *b)
{
    uint64_t head = atomic_load_explicit(&l->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&l->tail, memory_order_acquire);
    struct tg_log_slot *s;

    if (head - tail >= l->nslots) {
        atomic_fetch_add_explicit(&l->dropped, 1, memory_order_relaxed);
        return false;
    }
    s = &l->ring[head % l->nslots];
    s->level = (uint8_t)lv;
    tg_buf_init(b, s->text, TG_LOG_TEXT_CAP);
    return true;
}

/* 핵심 경로: 할당 금지 */
void tg_log_end(struct tg_log *l, const struct tg_buf *b)
{
    uint64_t head = atomic_load_explicit(&l->head, memory_order_relaxed);
    struct tg_log_slot *s = &l->ring[head % l->nslots];
    uint64_t one = 1;

    /* 잘렸으면 마지막 3바이트를 "..."로 바꿔 표시한다. */
    if (b->overflow && b->len >= 3) {
        s->text[b->len - 3] = '.';
        s->text[b->len - 2] = '.';
        s->text[b->len - 1] = '.';
    }
    s->len = (uint16_t)b->len;
    s->ts_ns = tg_now_ns();

    /* 규칙 1: head를 먼저 올리고 그다음에 깨운다. */
    atomic_store_explicit(&l->head, head + 1, memory_order_release);
    while (write(l->efd, &one, sizeof(one)) < 0 && errno == EINTR)
        ;
}

/* ---------- 소비자 ---------- */

static int fd_sink(void *ctx, const char *buf, size_t len)
{
    int fd = (int)(intptr_t)ctx;
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        off += (size_t)n;
    }
    return 0;
}

static const char *const LEVEL_NAME[] = {"err", "warn", "info", "debug"};
static const char LEVEL_PRIO[] = {'3', '4', '6', '7'};

/* 슬롯 하나를 출력 한 줄로 만든다. out에는 TG_LOG_LINE_MAX 바이트가 있어야 한다. */
static size_t format_line(const struct tg_log *l, const struct tg_log_slot *s, char *out)
{
    char *p = out;
    uint8_t lv = s->level > TG_LOG_DEBUG ? TG_LOG_DEBUG : s->level;
    size_t n;
    uint64_t real_ns = (uint64_t)((int64_t)s->ts_ns + l->real_minus_mono_ns);
    uint16_t len = s->len > TG_LOG_TEXT_CAP - 1 ? TG_LOG_TEXT_CAP - 1 : s->len;

    if (l->journald_prefix) {
        *p++ = '<';
        *p++ = LEVEL_PRIO[lv];
        *p++ = '>';
    }
    p += tg_log_fmt_time(p, real_ns);
    *p++ = ' ';
    n = strlen(LEVEL_NAME[lv]);
    memcpy(p, LEVEL_NAME[lv], n);
    p += n;
    *p++ = ' ';
    memcpy(p, s->text, len);
    p += len;
    *p++ = '\n';
    return (size_t)(p - out);
}

static void flush(struct tg_log *l, char *out, size_t *used)
{
    if (*used == 0)
        return;
    if (l->sink(l->sink_ctx, out, *used) != 0)
        atomic_fetch_add_explicit(&l->write_errors, 1, memory_order_relaxed);
    *used = 0;
}

/* 링버퍼를 완전히 비워 모음 버퍼에 이어 붙인다. 모음 버퍼가 차면 중간에 내보낸다. */
static void drain(struct tg_log *l, char *out, size_t *used)
{
    for (;;) {
        uint64_t tail = atomic_load_explicit(&l->tail, memory_order_relaxed);
        uint64_t head = atomic_load_explicit(&l->head, memory_order_acquire);
        const struct tg_log_slot *s;

        if (tail == head)
            break;
        s = &l->ring[tail % l->nslots];
        if (*used + TG_LOG_LINE_MAX > TG_LOG_BATCH_BYTES)
            flush(l, out, used);
        *used += format_line(l, s, out + *used);
        /* 다 복사한 칸만 생산자가 재사용하도록 release로 올린다. */
        atomic_store_explicit(&l->tail, tail + 1, memory_order_release);
    }
}

/* "로그 N건 버림" 같은 소비자가 직접 만드는 알림 줄 */
static void notice(struct tg_log *l, char *out, size_t *used, const char *pre, uint64_t n,
                   const char *post)
{
    struct tg_log_slot s;
    struct tg_buf b;

    memset(&s, 0, sizeof(s));
    s.level = TG_LOG_WARN;
    s.ts_ns = tg_now_ns();
    tg_buf_init(&b, s.text, TG_LOG_TEXT_CAP);
    tg_buf_str(&b, pre);
    tg_buf_u64(&b, n);
    tg_buf_str(&b, post);
    s.len = (uint16_t)b.len;
    if (*used + TG_LOG_LINE_MAX > TG_LOG_BATCH_BYTES)
        flush(l, out, used);
    *used += format_line(l, &s, out + *used);
}

static void report_counters(struct tg_log *l, char *out, size_t *used)
{
    uint64_t n = atomic_exchange_explicit(&l->dropped, 0, memory_order_relaxed);

    if (n > 0)
        notice(l, out, used, "로그 ", n, "건 버림");
    n = atomic_exchange_explicit(&l->write_errors, 0, memory_order_relaxed);
    if (n > 0)
        notice(l, out, used, "로그 쓰기 실패 ", n, "회");
}

static void *writer_main(void *arg)
{
    struct tg_log *l = arg;
    char out[TG_LOG_BATCH_BYTES];
    size_t used = 0;
    sigset_t all;

    /* 시그널은 메인 스레드만 받는다. */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    for (;;) {
        uint64_t v;
        ssize_t r = read(l->efd, &v, sizeof(v)); /* 카운터가 0이면 여기서 잠든다 */

        bool stopping;

        if (r < 0 && errno == EINTR)
            continue;
        /*
         * 정지 플래그를 비우기 전에 먼저 읽는다. 생산자는 마지막 줄과 버림 집계를 다 쓴 뒤에
         * 플래그를 올리므로, 플래그를 본 뒤의 이번 비우기가 그 이전의 모든 줄과 버림을 반영한다.
         * (비운 뒤에 플래그를 읽으면 그 사이에 생긴 버림 집계를 보고하지 못하고 끝날 수 있다.)
         */
        stopping = atomic_load_explicit(&l->stop, memory_order_acquire);
        /* 규칙 2: 카운터 값이 아니라 버퍼를 직접 확인해 빌 때까지 비운다. */
        drain(l, out, &used);
        report_counters(l, out, &used);
        flush(l, out, &used);
        if (stopping || r < 0)
            break; /* r < 0은 예상 밖의 오류다. 바쁜 반복을 피하려고 끝낸다 */
    }
    return NULL;
}

/* ---------- 시작과 종료 ---------- */

int tg_log_open(struct tg_log *l, const struct tg_log_opts *o)
{
    pthread_attr_t attr;
    struct timespec rt;
    int rc;

    if (o->nslots == 0 || o->sink == NULL)
        return -EINVAL;

    memset(l, 0, sizeof(*l));
    atomic_init(&l->head, 0);
    atomic_init(&l->tail, 0);
    atomic_init(&l->dropped, 0);
    atomic_init(&l->write_errors, 0);
    atomic_init(&l->stop, false);
    l->owned_fd = -1;
    l->nslots = o->nslots;
    l->sink = o->sink;
    l->sink_ctx = o->sink_ctx;
    l->journald_prefix = o->journald_prefix;

    l->ring = calloc(o->nslots, sizeof(*l->ring));
    if (l->ring == NULL)
        return -ENOMEM;
    l->efd = eventfd(0, EFD_CLOEXEC);
    if (l->efd < 0) {
        rc = -errno;
        free(l->ring);
        l->ring = NULL;
        return rc;
    }

    clock_gettime(CLOCK_REALTIME, &rt);
    l->real_minus_mono_ns =
        (int64_t)((uint64_t)rt.tv_sec * NS_PER_SEC + (uint64_t)rt.tv_nsec) - (int64_t)tg_now_ns();

    rc = pthread_attr_init(&attr);
    if (rc == 0) {
        rc = pthread_attr_setstacksize(&attr, TG_LOG_STACK_BYTES);
        if (rc == 0)
            rc = pthread_create(&l->thread, &attr, writer_main, l);
        pthread_attr_destroy(&attr);
    }
    if (rc != 0) {
        close(l->efd);
        free(l->ring);
        l->ring = NULL;
        return -rc;
    }
    l->started = true;
    return 0;
}

int tg_log_start(struct tg_log *l, const struct tg_config *cfg)
{
    struct tg_log_opts o;
    int fd = STDERR_FILENO;
    int owned = -1;
    int open_errno = 0;
    int rc;

    if (cfg->log_path[0] != '\0') {
        /*
         * 독자가 없는 FIFO는 O_NONBLOCK 없이 열면 독자가 나타날 때까지 open이 멈춰 표준 오류 대체에도
         * 닿지 못한다. 그래서 O_NONBLOCK으로 열고(독자가 없으면 ENXIO로 실패해 대체한다), 연 뒤에는
         * 해제해서 이후 쓰기는 기록 스레드가 기다리게 한다. 일반 파일에는 영향이 없다.
         */
        fd = open(cfg->log_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NONBLOCK, 0640);
        if (fd < 0) {
            open_errno = errno;
            fd = STDERR_FILENO; /* 로그 때문에 보호 기능이 꺼지지 않게 표준 오류로 대체한다 */
        } else {
            struct stat st;

            /* 일반 파일이 아니면(FIFO 등) 느린 독자에게 기록 스레드가 묶이므로 쓰지 않는다 */
            if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
                open_errno = EINVAL;
                close(fd);
                fd = STDERR_FILENO;
            } else {
                int fl = fcntl(fd, F_GETFL);

                if (fl >= 0)
                    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
                owned = fd;
            }
        }
    }

    memset(&o, 0, sizeof(o));
    o.nslots = cfg->log_ring_kb * 1024u / TG_LOG_SLOT_BYTES;
    o.sink = fd_sink;
    o.sink_ctx = (void *)(intptr_t)fd;
    o.journald_prefix = fd == STDERR_FILENO && getenv("JOURNAL_STREAM") != NULL;

    rc = tg_log_open(l, &o);
    if (rc != 0) {
        if (owned >= 0)
            close(owned);
        return rc;
    }
    l->owned_fd = owned;

    if (open_errno != 0) {
        struct tg_buf b;

        l->fell_back = true;
        if (tg_log_begin(l, TG_LOG_WARN, &b)) {
            tg_buf_str(&b, "log_path 열기 실패 errno=");
            tg_buf_u64(&b, (uint64_t)open_errno);
            tg_buf_str(&b, ", 표준 오류 사용");
            tg_log_end(l, &b);
        }
    }
    return 0;
}

void tg_log_stop(struct tg_log *l)
{
    uint64_t one = 1;

    if (!l->started)
        return;
    atomic_store_explicit(&l->stop, true, memory_order_release);
    while (write(l->efd, &one, sizeof(one)) < 0 && errno == EINTR)
        ;
    pthread_join(l->thread, NULL);
    l->started = false;

    close(l->efd);
    if (l->owned_fd >= 0)
        close(l->owned_fd);
    l->owned_fd = -1;
    free(l->ring);
    l->ring = NULL;
}
