/*
 * test_log.c - 로그 링버퍼와 기록 스레드 단위 테스트
 *
 * 출력 대상을 시험용 수집기로 바꿔 끼워 내용, 순서, 버림, 쓰기 실패를 확인한다.
 * 수집기는 기록 스레드에서만 호출되고, 테스트 본문은 tg_log_stop(스레드 합치기) 뒤에 결과를
 * 읽거나 atomic 카운터를 기다린다.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "tg_test.h"

/* ---------- 시험용 출력 대상 ---------- */

struct col {
    char buf[1 << 16];
    size_t len;
    bool store;

    _Atomic uint64_t calls;
    _Atomic uint64_t lines;
    _Atomic uint64_t last_seq;
    _Atomic uint64_t seq_count;
    uint64_t dropped_reported;
    bool order_ok;

    atomic_int gate;      /* 0이면 sink가 열릴 때까지 기다린다 */
    int delay_us;         /* 호출마다 지연 */
    int fail_remaining;   /* 앞으로 이 횟수만큼 실패를 돌려준다 */
    int fail_errno;
};

static void col_init(struct col *c)
{
    memset(c, 0, sizeof(*c));
    atomic_init(&c->calls, 0);
    atomic_init(&c->lines, 0);
    atomic_init(&c->last_seq, 0);
    atomic_init(&c->seq_count, 0);
    atomic_init(&c->gate, 1);
    c->store = true;
    c->order_ok = true;
}

static void col_parse_line(struct col *c, const char *line, size_t n)
{
    char tmp[400];
    const char *p;

    if (n >= sizeof(tmp))
        n = sizeof(tmp) - 1;
    memcpy(tmp, line, n);
    tmp[n] = '\0';

    p = strstr(tmp, " n=");
    if (p != NULL) {
        uint64_t v = strtoull(p + 3, NULL, 10);

        if (atomic_load(&c->seq_count) > 0 && v <= atomic_load(&c->last_seq))
            c->order_ok = false;
        atomic_store(&c->last_seq, v);
        atomic_fetch_add(&c->seq_count, 1);
    }
    p = strstr(tmp, "로그 ");
    if (p != NULL && strstr(p, "건 버림") != NULL)
        c->dropped_reported += strtoull(p + strlen("로그 "), NULL, 10);
}

static int col_sink(void *ctx, const char *buf, size_t len)
{
    struct col *c = ctx;
    size_t start = 0;

    atomic_fetch_add(&c->calls, 1);
    while (atomic_load(&c->gate) == 0)
        usleep(1000);
    if (c->delay_us > 0)
        usleep((useconds_t)c->delay_us);
    if (c->fail_remaining > 0) {
        c->fail_remaining--;
        return -c->fail_errno;
    }
    if (c->store && c->len + len < sizeof(c->buf)) {
        memcpy(c->buf + c->len, buf, len);
        c->len += len;
        c->buf[c->len] = '\0';
    }
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            col_parse_line(c, buf + start, i - start);
            atomic_fetch_add(&c->lines, 1);
            start = i + 1;
        }
    }
    return 0;
}

static bool wait_ge(_Atomic uint64_t *p, uint64_t v)
{
    for (int i = 0; i < 400; i++) {
        if (atomic_load(p) >= v)
            return true;
        usleep(10000);
    }
    return false;
}

/* 짧은 간격으로 확인하는 대기. 최대 약 2초. */
static bool wait_fast(_Atomic uint64_t *p, uint64_t v)
{
    for (int i = 0; i < 20000; i++) {
        if (atomic_load(p) >= v)
            return true;
        usleep(100);
    }
    return false;
}

static int open_log(struct tg_log *l, struct col *c, uint32_t nslots, bool prefix)
{
    struct tg_log_opts o;

    memset(&o, 0, sizeof(o));
    o.nslots = nslots;
    o.sink = col_sink;
    o.sink_ctx = c;
    o.journald_prefix = prefix;
    return tg_log_open(l, &o);
}

/* 줄 하나를 쓴다. 가득 차 있으면 false. */
static bool put(struct tg_log *l, enum tg_log_level lv, const char *key, uint64_t v)
{
    struct tg_buf b;

    if (!tg_log_begin(l, lv, &b))
        return false;
    tg_buf_kv_u64(&b, key, v);
    tg_log_end(l, &b);
    return true;
}

/* ---------- 시각 문자열 ---------- */

static void test_fmt_time(void)
{
    char t[32];

    TG_CHECK_EQ_U64(tg_log_fmt_time(t, 0), TG_LOG_TS_LEN);
    TG_CHECK(strcmp(t, "1970-01-01T00:00:00.000Z") == 0);

    tg_log_fmt_time(t, 951868800ull * 1000000000ull);
    TG_CHECK(strcmp(t, "2000-03-01T00:00:00.000Z") == 0);

    tg_log_fmt_time(t, 1700000000ull * 1000000000ull + 123456789ull);
    TG_CHECK(strcmp(t, "2023-11-14T22:13:20.123Z") == 0);

    /* 윤일 */
    tg_log_fmt_time(t, 1709164800ull * 1000000000ull);
    TG_CHECK(strcmp(t, "2024-02-29T00:00:00.000Z") == 0);
    tg_log_fmt_time(t, 1709251199ull * 1000000000ull + 999999999ull);
    TG_CHECK(strcmp(t, "2024-02-29T23:59:59.999Z") == 0);

    tg_log_fmt_time(t, 4102444799ull * 1000000000ull);
    TG_CHECK(strcmp(t, "2099-12-31T23:59:59.000Z") == 0);
}

/* ---------- 기본 동작 ---------- */

static void test_open_invalid(void)
{
    struct tg_log l;
    struct col c;
    struct tg_log_opts o;

    col_init(&c);
    memset(&o, 0, sizeof(o));
    o.sink = col_sink;
    o.sink_ctx = &c;
    o.nslots = 0;
    TG_CHECK(tg_log_open(&l, &o) == -EINVAL);
    o.nslots = 8;
    o.sink = NULL;
    TG_CHECK(tg_log_open(&l, &o) == -EINVAL);
}

static void test_basic_order_and_format(void)
{
    struct tg_log l;
    struct col c;

    col_init(&c);
    TG_CHECK(open_log(&l, &c, 16, false) == 0);
    for (uint64_t i = 1; i <= 5; i++)
        TG_CHECK(put(&l, TG_LOG_INFO, "msg", i));
    TG_CHECK(put(&l, TG_LOG_ERR, "bad", 9));
    TG_CHECK(wait_ge(&c.lines, 6));
    tg_log_stop(&l);

    /* 형식: "<시각 24자> <레벨> <본문>\n" */
    TG_CHECK(c.buf[4] == '-' && c.buf[10] == 'T' && c.buf[23] == 'Z' && c.buf[24] == ' ');
    TG_CHECK(strstr(c.buf, "Z info msg=1\n") != NULL);
    TG_CHECK(strstr(c.buf, "Z err bad=9\n") != NULL);
    /* 순서 */
    {
        const char *p1 = strstr(c.buf, "msg=1");
        const char *p3 = strstr(c.buf, "msg=3");
        const char *p5 = strstr(c.buf, "msg=5");

        TG_CHECK(p1 != NULL && p3 != NULL && p5 != NULL && p1 < p3 && p3 < p5);
    }
    TG_CHECK_EQ_U64(atomic_load(&l.dropped), 0);
}

static void test_journald_prefix(void)
{
    struct tg_log l;
    struct col c;

    col_init(&c);
    TG_CHECK(open_log(&l, &c, 16, true) == 0);
    TG_CHECK(put(&l, TG_LOG_INFO, "a", 1));
    TG_CHECK(put(&l, TG_LOG_ERR, "b", 2));
    TG_CHECK(put(&l, TG_LOG_WARN, "c", 3));
    TG_CHECK(put(&l, TG_LOG_DEBUG, "d", 4));
    tg_log_stop(&l);

    TG_CHECK(strstr(c.buf, "<6>20") != NULL);
    TG_CHECK(strstr(c.buf, "<3>20") != NULL);
    TG_CHECK(strstr(c.buf, "<4>20") != NULL);
    TG_CHECK(strstr(c.buf, "<7>20") != NULL);
    TG_CHECK(strncmp(c.buf, "<6>", 3) == 0);
}

static void test_truncation(void)
{
    struct tg_log l;
    struct col c;
    struct tg_buf b;
    char *nl;
    size_t body;

    col_init(&c);
    TG_CHECK(open_log(&l, &c, 8, false) == 0);
    TG_CHECK(tg_log_begin(&l, TG_LOG_INFO, &b));
    for (int i = 0; i < 400; i++)
        tg_buf_char(&b, 'x');
    tg_log_end(&l, &b);
    TG_CHECK(put(&l, TG_LOG_INFO, "after", 1)); /* 잘린 줄이 다음 줄을 망치지 않는다 */
    tg_log_stop(&l);

    nl = strchr(c.buf, '\n');
    TG_CHECK(nl != NULL);
    if (nl != NULL) {
        /* 접두 "시각(24) 레벨(info) 공백 두 개" = 24 + 1 + 4 + 1 = 30 */
        body = (size_t)(nl - c.buf) - 30;
        TG_CHECK_EQ_U64(body, 243);
        TG_CHECK(nl[-1] == '.' && nl[-2] == '.' && nl[-3] == '.' && nl[-4] == 'x');
    }
    TG_CHECK(strstr(c.buf, "info after=1\n") != NULL);
}

/* ---------- 가득 참과 버림 ---------- */

static void test_full_and_dropped(void)
{
    struct tg_log l;
    struct col c;

    col_init(&c);
    atomic_store(&c.gate, 0); /* 출력이 막힌 상황 */
    TG_CHECK(open_log(&l, &c, 4, false) == 0);

    TG_CHECK(put(&l, TG_LOG_INFO, "first", 1));
    TG_CHECK(wait_ge(&c.calls, 1)); /* 첫 줄은 이미 꺼내져 sink에서 막혀 있다 */

    for (uint64_t i = 1; i <= 4; i++)
        TG_CHECK(put(&l, TG_LOG_INFO, "fill", i)); /* 빈 칸 4개를 채운다 */
    TG_CHECK(!put(&l, TG_LOG_INFO, "over", 1));    /* 가득 참: 버린다 */
    TG_CHECK(!put(&l, TG_LOG_INFO, "over", 2));
    TG_CHECK_EQ_U64(atomic_load(&l.dropped), 2);

    atomic_store(&c.gate, 1);
    tg_log_stop(&l);

    TG_CHECK(strstr(c.buf, "first=1") != NULL);
    TG_CHECK(strstr(c.buf, "fill=1") != NULL);
    TG_CHECK(strstr(c.buf, "fill=4") != NULL);
    TG_CHECK(strstr(c.buf, "over=") == NULL);
    TG_CHECK(strstr(c.buf, "로그 2건 버림") != NULL);
    TG_CHECK_EQ_U64(atomic_load(&c.lines), 6); /* first + fill 4 + 알림 1 */
    TG_CHECK_EQ_U64(atomic_load(&l.dropped), 0);

    /* 비운 뒤에는 다시 쓸 수 있다 */
}

static void test_reuse_after_drain(void)
{
    struct tg_log l;
    struct col c;

    col_init(&c);
    TG_CHECK(open_log(&l, &c, 4, false) == 0);
    for (uint64_t i = 1; i <= 20; i++) {
        TG_CHECK(put(&l, TG_LOG_INFO, "r", i));
        TG_CHECK(wait_ge(&c.lines, i)); /* 하나씩 비우면 가득 찰 일이 없다 */
    }
    TG_CHECK_EQ_U64(atomic_load(&l.dropped), 0);
    tg_log_stop(&l);
}

/* ---------- 쓰기 실패와 모아 쓰기 ---------- */

static void test_write_error(void)
{
    struct tg_log l;
    struct col c;

    col_init(&c);
    c.fail_remaining = 1;
    c.fail_errno = ENOSPC;
    TG_CHECK(open_log(&l, &c, 16, false) == 0);

    TG_CHECK(put(&l, TG_LOG_INFO, "lost", 1));
    TG_CHECK(wait_ge(&l.write_errors, 1)); /* 첫 모음은 실패로 버려졌다 */
    TG_CHECK(put(&l, TG_LOG_INFO, "kept", 2));
    tg_log_stop(&l);

    TG_CHECK(strstr(c.buf, "lost=1") == NULL);
    TG_CHECK(strstr(c.buf, "kept=2") != NULL);
    TG_CHECK(strstr(c.buf, "로그 쓰기 실패 1회") != NULL);
}

static void test_batching(void)
{
    struct tg_log l;
    struct col c;

    col_init(&c);
    atomic_store(&c.gate, 0);
    TG_CHECK(open_log(&l, &c, 64, false) == 0);

    TG_CHECK(put(&l, TG_LOG_INFO, "n", 1));
    TG_CHECK(wait_ge(&c.calls, 1)); /* 첫 줄만 단독으로 막혀 있다 */
    for (uint64_t i = 2; i <= 40; i++)
        TG_CHECK(put(&l, TG_LOG_INFO, "n", i));
    atomic_store(&c.gate, 1);
    tg_log_stop(&l);

    TG_CHECK_EQ_U64(atomic_load(&c.lines), 40);
    TG_CHECK(c.order_ok);
    /* 쌓인 39줄은 줄마다가 아니라 모아서 나간다: 줄당 약 40바이트라 4 KiB에 100줄 이상 들어간다 */
    TG_CHECK(atomic_load(&c.calls) <= 3);
}

/* ---------- 동시성 ---------- */

static void test_stress_order_and_accounting(void)
{
    struct tg_log l;
    struct col c;
    const uint64_t attempts = 300000;
    uint64_t ok = 0;

    col_init(&c);
    c.store = false;
    TG_CHECK(open_log(&l, &c, 64, false) == 0);
    for (uint64_t i = 1; i <= attempts; i++) {
        if (put(&l, TG_LOG_INFO, "n", i))
            ok++;
    }
    tg_log_stop(&l);

    /* 도착한 줄은 번호가 순서대로이고, 쓴 것 = 도착 + 버림이다 */
    TG_CHECK(c.order_ok);
    TG_CHECK_EQ_U64(atomic_load(&c.seq_count), ok);
    TG_CHECK_EQ_U64(atomic_load(&c.seq_count) + c.dropped_reported, attempts);
    TG_CHECK_EQ_U64(atomic_load(&l.dropped), 0);
    TG_CHECK(ok > 0);
}

static void test_stress_slow_consumer(void)
{
    struct tg_log l;
    struct col c;
    const uint64_t attempts = 20000;
    uint64_t ok = 0;

    col_init(&c);
    c.store = false;
    c.delay_us = 50; /* 느린 출력 */
    TG_CHECK(open_log(&l, &c, 32, false) == 0);
    for (uint64_t i = 1; i <= attempts; i++) {
        if (put(&l, TG_LOG_INFO, "n", i))
            ok++;
    }
    tg_log_stop(&l);

    TG_CHECK(c.order_ok);
    TG_CHECK_EQ_U64(atomic_load(&c.seq_count), ok);
    TG_CHECK_EQ_U64(atomic_load(&c.seq_count) + c.dropped_reported, attempts);
    TG_CHECK(c.dropped_reported > 0); /* 느린 출력이라 일부는 버려져야 이 시험의 의미가 있다 */
}

static void test_no_stuck_line(void)
{
    struct tg_log l;
    struct col c;
    const uint64_t total = 3000;

    /*
     * 생산자가 드문드문 쓰면 소비자가 "비었다"고 확인한 직후에 줄이 들어오는 겹침이 자주 생긴다.
     * 마지막 줄을 stop 없이도 받아야 한다(규칙 1, 2).
     */
    col_init(&c);
    c.store = false;
    c.delay_us = 100;
    TG_CHECK(open_log(&l, &c, 256, false) == 0);
    for (uint64_t i = 1; i <= total; i++) {
        TG_CHECK(put(&l, TG_LOG_INFO, "n", i));
        if (i % 3 == 0) {
            usleep((useconds_t)((i % 7) * 40));
            /* 쉬는 동안 방금 쓴 줄까지 도착해야 한다. 갇힌 줄이 있으면 여기서 드러난다. */
            TG_CHECK(wait_fast(&c.last_seq, i));
        }
    }
    TG_CHECK(wait_ge(&c.last_seq, total));
    TG_CHECK_EQ_U64(atomic_load(&c.seq_count), total);
    TG_CHECK(c.order_ok);
    tg_log_stop(&l);
}

/* ---------- 설정에 따른 시작과 종료 ---------- */

static char tmpdir[64];

static void read_file(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY);
    ssize_t n = 0;

    out[0] = '\0';
    if (fd >= 0) {
        n = read(fd, out, cap - 1);
        close(fd);
    }
    out[n > 0 ? n : 0] = '\0';
}

static void test_start_file(void)
{
    struct tg_log l;
    struct tg_config cfg;
    char path[128];
    char text[2048];
    struct tg_buf b;

    tg_config_defaults(&cfg);
    cfg.log_ring_kb = 4;
    snprintf(path, sizeof(path), "%s/out.log", tmpdir);
    snprintf(cfg.log_path, sizeof(cfg.log_path), "%s", path);

    TG_CHECK(tg_log_start(&l, &cfg) == 0);
    TG_CHECK_EQ_U64(l.nslots, 16); /* 4 KiB / 256 */
    TG_CHECK(!l.fell_back);
    TG_CHECK(tg_log_begin(&l, TG_LOG_INFO, &b));
    tg_buf_str(&b, "hello file");
    tg_log_end(&l, &b);
    tg_log_stop(&l);

    read_file(path, text, sizeof(text));
    TG_CHECK(strstr(text, " info hello file\n") != NULL);

    /* 다시 열면 이어 쓴다 */
    TG_CHECK(tg_log_start(&l, &cfg) == 0);
    TG_CHECK(tg_log_begin(&l, TG_LOG_INFO, &b));
    tg_buf_str(&b, "second");
    tg_log_end(&l, &b);
    tg_log_stop(&l);
    read_file(path, text, sizeof(text));
    TG_CHECK(strstr(text, "hello file") != NULL && strstr(text, "second") != NULL);

    unlink(path);
}

static void test_start_fallback(void)
{
    struct tg_log l;
    struct tg_config cfg;
    char path[128];
    char text[2048];
    struct tg_buf b;
    int saved;
    int tmp;

    tg_config_defaults(&cfg);
    /* 환경에 따라 달라지지 않도록, 방금 만든 임시 디렉터리 아래 만들지 않은 하위 디렉터리를 쓴다 */
    snprintf(cfg.log_path, sizeof(cfg.log_path), "%s/no_such_dir/x.log", tmpdir);

    /* 표준 오류를 임시 파일로 돌려 대체 동작을 관찰한다 */
    snprintf(path, sizeof(path), "%s/stderr.txt", tmpdir);
    tmp = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    saved = dup(STDERR_FILENO);
    TG_CHECK(tmp >= 0 && saved >= 0);
    dup2(tmp, STDERR_FILENO);
    close(tmp);

    TG_CHECK(tg_log_start(&l, &cfg) == 0);
    TG_CHECK(l.fell_back);
    TG_CHECK(tg_log_begin(&l, TG_LOG_INFO, &b));
    tg_buf_str(&b, "still logging");
    tg_log_end(&l, &b);
    tg_log_stop(&l);

    dup2(saved, STDERR_FILENO);
    close(saved);

    read_file(path, text, sizeof(text));
    TG_CHECK(strstr(text, "log_path 열기 실패 errno=2, 표준 오류 사용") != NULL);
    TG_CHECK(strstr(text, "still logging") != NULL);
    unlink(path);
}

static void test_start_fifo_without_reader(void)
{
    struct tg_log l;
    struct tg_config cfg;
    char path[128];
    char errpath[128];
    int saved;
    int tmp;
    int rc;

    tg_config_defaults(&cfg);
    snprintf(path, sizeof(path), "%s/fifo", tmpdir);
    TG_CHECK(mkfifo(path, 0600) == 0);
    snprintf(cfg.log_path, sizeof(cfg.log_path), "%s", path);

    snprintf(errpath, sizeof(errpath), "%s/stderr_fifo.txt", tmpdir);
    tmp = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    saved = dup(STDERR_FILENO);
    TG_CHECK(tmp >= 0 && saved >= 0);
    dup2(tmp, STDERR_FILENO);
    close(tmp);

    /* 독자 없는 FIFO를 열 때 멈추면 안 된다. 멈추면 5초 뒤 알람(SIGALRM)으로 테스트가 실패한다. */
    alarm(5);
    rc = tg_log_start(&l, &cfg);
    alarm(0);
    TG_CHECK(rc == 0);
    TG_CHECK(l.fell_back);
    tg_log_stop(&l);

    dup2(saved, STDERR_FILENO);
    close(saved);
    unlink(path);
    unlink(errpath);
}

/* 독자가 있는 FIFO도 일반 파일이 아니므로 표준 오류로 대체한다 */
static void test_start_fifo_with_reader(void)
{
    struct tg_log l;
    struct tg_config cfg;
    char path[128];
    char errpath[128];
    int saved;
    int tmp;
    int rd;

    tg_config_defaults(&cfg);
    snprintf(path, sizeof(path), "%s/fifo2", tmpdir);
    TG_CHECK(mkfifo(path, 0600) == 0);
    snprintf(cfg.log_path, sizeof(cfg.log_path), "%s", path);
    rd = open(path, O_RDONLY | O_NONBLOCK);
    TG_CHECK(rd >= 0);

    snprintf(errpath, sizeof(errpath), "%s/stderr_fifo2.txt", tmpdir);
    tmp = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    saved = dup(STDERR_FILENO);
    TG_CHECK(tmp >= 0 && saved >= 0);
    dup2(tmp, STDERR_FILENO);
    close(tmp);

    TG_CHECK(tg_log_start(&l, &cfg) == 0);
    TG_CHECK(l.fell_back);
    tg_log_stop(&l);

    dup2(saved, STDERR_FILENO);
    close(saved);
    close(rd);
    unlink(path);
    unlink(errpath);
}

static void test_stop_idempotent(void)
{
    struct tg_log l;
    struct col c;

    col_init(&c);
    TG_CHECK(open_log(&l, &c, 8, false) == 0);
    tg_log_stop(&l);
    tg_log_stop(&l); /* 두 번 불러도 안전 */
    TG_CHECK(open_log(&l, &c, 8, false) == 0); /* 같은 구조체로 다시 열 수 있다 */
    TG_CHECK(put(&l, TG_LOG_INFO, "again", 1));
    tg_log_stop(&l);
    TG_CHECK(strstr(c.buf, "again=1") != NULL);
}

int main(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/tg_log_XXXXXX");
    if (mkdtemp(tmpdir) == NULL) {
        perror("mkdtemp");
        return 2;
    }

    TG_RUN(test_fmt_time);
    TG_RUN(test_open_invalid);
    TG_RUN(test_basic_order_and_format);
    TG_RUN(test_journald_prefix);
    TG_RUN(test_truncation);
    TG_RUN(test_full_and_dropped);
    TG_RUN(test_reuse_after_drain);
    TG_RUN(test_write_error);
    TG_RUN(test_batching);
    TG_RUN(test_stress_order_and_accounting);
    TG_RUN(test_stress_slow_consumer);
    TG_RUN(test_no_stuck_line);
    TG_RUN(test_start_file);
    TG_RUN(test_start_fallback);
    TG_RUN(test_start_fifo_without_reader);
    TG_RUN(test_start_fifo_with_reader);
    TG_RUN(test_stop_idempotent);

    rmdir(tmpdir);
    return tg_test_finish();
}
