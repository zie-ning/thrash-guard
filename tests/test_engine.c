/*
 * test_engine.c - 샘플 수집, 엔진, 이벤트 루프 단위 테스트
 *
 * 가짜 /proc 파일(PSI, meminfo)과 가짜 cgroup 디렉터리를 임시 폴더에 만들고, 틱 시각을
 * 직접 넘겨서 실제 시계 없이 동작을 확인한다. 설정은 윈도 1초(4칸), 틱 250 ms로 줄여 쓴다.
 * 실제로 프로세스가 죽는지는 VM 실험에서 확인한다(여기서는 cgroup.kill에 1이 쓰이는지만 본다).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine.h"
#include "loop.h"
#include "mono.h"
#include "tg_test.h"

#define MIB(x) ((uint64_t)(x) << 20)
#define TICK_NS 250000000ull
#define T0 (1000ull * 1000000000ull)
#define FULL60_US 150000ull /* 250 ms 중 60% */

static char root[64];
static char psi_path[128];
static char mem_path[128];
static char parent[128];
static int sig_fd = -1;

/* ---------- 시험용 로그 출력 ---------- */

static char logbuf[1 << 16];
static size_t loglen;

static int test_sink(void *ctx, const char *buf, size_t len)
{
    (void)ctx;
    if (loglen + len < sizeof(logbuf)) {
        memcpy(logbuf + loglen, buf, len);
        loglen += len;
        logbuf[loglen] = '\0';
    }
    return 0;
}

/* ---------- 가짜 파일 ---------- */

static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    TG_CHECK(fd >= 0);
    if (fd >= 0) {
        TG_CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
        close(fd);
    }
}

static void set_full(uint64_t total_us)
{
    char s[256];

    snprintf(s, sizeof(s),
             "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
             "full avg10=0.00 avg60=0.00 avg300=0.00 total=%llu\n",
             (unsigned long long)total_us);
    write_file(psi_path, s);
}

static void set_avail_kib(uint64_t kib)
{
    char s[256];

    snprintf(s, sizeof(s),
             "MemTotal:        1048576 kB\nMemFree:          100000 kB\n"
             "MemAvailable:    %llu kB\n",
             (unsigned long long)kib);
    write_file(mem_path, s);
}

static void cg_path(char *out, size_t cap, const char *name, const char *file)
{
    snprintf(out, cap, "%s/%s/%s", parent, name, file);
}

static void mk_cg(const char *name, uint64_t bytes)
{
    char p[256];
    char v[32];

    snprintf(p, sizeof(p), "%s/%s", parent, name);
    TG_CHECK(mkdir(p, 0700) == 0);
    cg_path(p, sizeof(p), name, "memory.pressure");
    write_file(p, "");
    cg_path(p, sizeof(p), name, "cgroup.kill");
    write_file(p, "");
    cg_path(p, sizeof(p), name, "memory.current");
    snprintf(v, sizeof(v), "%llu\n", (unsigned long long)bytes);
    write_file(p, v);
}

static void set_current(const char *name, uint64_t bytes)
{
    char p[256];
    char v[32];

    cg_path(p, sizeof(p), name, "memory.current");
    snprintf(v, sizeof(v), "%llu\n", (unsigned long long)bytes);
    write_file(p, v);
}

static void read_kill(const char *name, char *out, size_t cap)
{
    char p[256];
    int fd;
    ssize_t n;

    cg_path(p, sizeof(p), name, "cgroup.kill");
    fd = open(p, O_RDONLY);
    TG_CHECK(fd >= 0);
    n = fd >= 0 ? read(fd, out, cap - 1) : 0;
    out[n < 0 ? 0 : n] = '\0';
    if (fd >= 0)
        close(fd);
}

/* ---------- 시험 환경 ---------- */

static struct tg_config cfg;
static struct tg_log lg;
static struct tg_scanner sc;
static struct tg_engine eng;
static uint64_t now_ns;
static uint64_t full_us;
static bool env_up;

/* A는 400 MiB, B는 50 MiB, 여유 메모리는 4.9%. 시스템 압력 누적은 0에서 시작한다. */
static void env_up_with(bool dry_run)
{
    struct tg_log_opts o;
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "rm -rf %s/cg && mkdir %s/cg", root, root);
    TG_CHECK(system(cmd) == 0);
    mk_cg("A", MIB(400));
    mk_cg("B", MIB(50));
    full_us = 0;
    set_full(full_us);
    set_avail_kib(50 * 1024);

    tg_config_defaults(&cfg);
    snprintf(cfg.target_parent, sizeof(cfg.target_parent), "%s", parent);
    cfg.sustain_sec = 1;
    cfg.sample_interval_ms = 250;
    cfg.dry_run = dry_run;

    loglen = 0;
    logbuf[0] = '\0';
    memset(&o, 0, sizeof(o));
    o.nslots = 64;
    o.sink = test_sink;
    TG_CHECK(tg_log_open(&lg, &o) == 0);
    TG_CHECK(tg_scanner_start(&sc, parent, 60000) == 0);
    TG_CHECK(tg_engine_init(&eng, &cfg, &lg, &sc, psi_path, mem_path) == 0);
    now_ns = T0;
    env_up = true;
}

/* 로그를 모두 내보낸 뒤 정리한다. 이후 logbuf를 읽는다. */
static void env_down(void)
{
    if (!env_up)
        return;
    tg_engine_close(&eng);
    tg_scanner_stop(&sc);
    tg_log_stop(&lg);
    env_up = false;
}

/* 압력을 주며 틱을 n번 돌린다. pressure_us는 한 틱에 늘어나는 full 누적(us). */
static void ticks(int n, uint64_t pressure_us)
{
    for (int i = 0; i < n; i++) {
        full_us += pressure_us;
        set_full(full_us);
        tg_engine_tick(&eng, now_ns);
        now_ns += TICK_NS;
    }
}

/* ---------- 샘플 수집 ---------- */

static void test_sample_collect(void)
{
    struct tg_reader r;
    static struct tg_sample_buf sb;
    const struct tg_targets *t;

    env_up_with(true);
    TG_CHECK(tg_reader_open(&r, psi_path, mem_path) == 0);
    t = tg_scanner_acquire(&sc);
    TG_CHECK(tg_sample_collect(&r, t, 5, &sb) == 0);
    TG_CHECK_EQ_U64(sb.s.ncand, 2);
    TG_CHECK_EQ_U64(sb.s.mem_total_bytes, MIB(1024));
    TG_CHECK_EQ_U64(sb.s.mem_available_bytes, MIB(50));
    TG_CHECK_EQ_U64(sb.s.sys_full_total_us, 0);
    TG_CHECK_EQ_U64(sb.s.now_ns, 5);
    TG_CHECK_EQ_U64(sb.read_failed, 0);
    {
        uint64_t total = 0;

        for (uint32_t i = 0; i < sb.s.ncand; i++)
            total += sb.cand[i].mem_bytes;
        TG_CHECK_EQ_U64(total, MIB(450));
    }
    tg_scanner_release(&sc);

    /* 읽을 수 없는 후보는 그 후보만 빠진다 */
    set_current("B", 0);
    {
        char p[256];

        cg_path(p, sizeof(p), "B", "memory.current");
        write_file(p, "max\n");
    }
    t = tg_scanner_acquire(&sc);
    TG_CHECK(tg_sample_collect(&r, t, 6, &sb) == 0);
    TG_CHECK_EQ_U64(sb.s.ncand, 1);
    TG_CHECK_EQ_U64(sb.read_failed, 1);
    tg_scanner_release(&sc);

    /* 시스템 값이 이상하면 실패한다 */
    write_file(mem_path, "MemTotal: 1048576 kB\n");
    t = tg_scanner_acquire(&sc);
    TG_CHECK(tg_sample_collect(&r, t, 7, &sb) == -EINVAL);
    tg_scanner_release(&sc);
    write_file(psi_path, "garbage\n");
    set_avail_kib(1);
    t = tg_scanner_acquire(&sc);
    TG_CHECK(tg_sample_collect(&r, t, 8, &sb) != 0);
    tg_scanner_release(&sc);

    tg_reader_close(&r);
    env_down();
}

/* ---------- 엔진 ---------- */

static void test_trigger_schedule(void)
{
    env_up_with(true);
    TG_CHECK(!tg_engine_watching(&eng));
    tg_engine_tick(&eng, now_ns); /* 관찰 중이 아니면 무시 */
    TG_CHECK_EQ_U64(eng.st.ticks, 0);

    tg_engine_trigger(&eng, now_ns);
    TG_CHECK(tg_engine_watching(&eng));
    TG_CHECK_EQ_U64(tg_engine_deadline(&eng), now_ns); /* 첫 틱은 바로 */
    tg_engine_trigger(&eng, now_ns + 1); /* 관찰 중 트리거는 무시 */
    TG_CHECK_EQ_U64(eng.st.triggers, 1);

    tg_engine_tick(&eng, now_ns);
    TG_CHECK_EQ_U64(tg_engine_deadline(&eng), now_ns + TICK_NS);
    env_down();
}

static void test_calm(void)
{
    env_up_with(true);
    tg_engine_trigger(&eng, now_ns);
    ticks(5, 0); /* 기준점 1 + 윈도 4칸. 압력 0이라 진정 */
    TG_CHECK(!tg_engine_watching(&eng));
    TG_CHECK_EQ_U64(eng.st.calms, 1);
    TG_CHECK_EQ_U64(eng.st.dry_kills, 0);
    env_down();
    TG_CHECK(strstr(logbuf, "judge calm") != NULL);
    TG_CHECK(strstr(logbuf, "trigger watch start") != NULL);
}

static void test_dry_run_kill(void)
{
    char buf[8];

    env_up_with(true);
    tg_engine_trigger(&eng, now_ns);
    ticks(5, FULL60_US);
    TG_CHECK(!tg_engine_watching(&eng));
    TG_CHECK_EQ_U64(eng.st.dry_kills, 1);
    TG_CHECK_EQ_U64(eng.st.kills, 0);
    read_kill("A", buf, sizeof(buf));
    TG_CHECK(strcmp(buf, "") == 0); /* dry-run은 쓰지 않는다 */

    /* 가상 종료도 쿨다운을 소모한다: 다음 관찰은 쿨다운으로 건너뛴다 */
    tg_engine_trigger(&eng, now_ns);
    ticks(5, FULL60_US);
    TG_CHECK_EQ_U64(eng.st.dry_kills, 1);
    TG_CHECK_EQ_U64(eng.st.skips, 1);
    TG_CHECK(tg_engine_watching(&eng)); /* 쿨다운이 끝나기를 기다리며 계속 관찰 */
    env_down();

    TG_CHECK(strstr(logbuf, "judge kill cand=A dry_run=1 result=0") != NULL);
    TG_CHECK(strstr(logbuf, "growing=0") != NULL);
    TG_CHECK(strstr(logbuf, "judge cand name=A") != NULL);
    TG_CHECK(strstr(logbuf, "judge cand name=B") != NULL);
    TG_CHECK(strstr(logbuf, "judge skip reason=cooldown") != NULL);
}

static void test_real_kill(void)
{
    char a[8];
    char b[8];

    env_up_with(false);
    tg_engine_trigger(&eng, now_ns);
    ticks(5, FULL60_US);
    TG_CHECK_EQ_U64(eng.st.kills, 1);
    TG_CHECK_EQ_U64(eng.st.dry_kills, 0);
    read_kill("A", a, sizeof(a));
    read_kill("B", b, sizeof(b));
    TG_CHECK(strcmp(a, "1") == 0);
    TG_CHECK(strcmp(b, "") == 0); /* 다른 후보는 건드리지 않는다 */
    env_down();
    TG_CHECK(strstr(logbuf, "judge kill cand=A dry_run=0 result=0") != NULL);
}

static void test_kill_failure_keeps_policy(void)
{
    char p[256];
    char a[8];

    env_up_with(false);
    cg_path(p, sizeof(p), "A", "cgroup.kill");
    TG_CHECK(unlink(p) == 0); /* 이 커널에는 cgroup.kill이 없는 상황 */

    tg_engine_trigger(&eng, now_ns);
    ticks(5, FULL60_US);
    TG_CHECK_EQ_U64(eng.st.kill_errors, 1);
    TG_CHECK_EQ_U64(eng.st.kills, 0);

    /* 실패는 쿨다운을 소모하지 않으므로 파일이 생기면 다음 관찰에서 바로 종료한다 */
    write_file(p, "");
    tg_engine_trigger(&eng, now_ns);
    ticks(5, FULL60_US);
    TG_CHECK_EQ_U64(eng.st.kills, 1);
    read_kill("A", a, sizeof(a));
    TG_CHECK(strcmp(a, "1") == 0);
    env_down();
    TG_CHECK(strstr(logbuf, "judge kill cand=A dry_run=0 result=-95") != NULL); /* -ENOTSUP */
}

static void test_mem_available_guard(void)
{
    env_up_with(false);
    set_avail_kib(600 * 1024); /* 여유가 충분하다 */
    tg_engine_trigger(&eng, now_ns);
    ticks(5, FULL60_US);
    TG_CHECK_EQ_U64(eng.st.kills, 0);
    TG_CHECK_EQ_U64(eng.st.skips, 1);
    TG_CHECK(tg_engine_watching(&eng));
    env_down();
    TG_CHECK(strstr(logbuf, "judge skip reason=mem_available") != NULL);
}

static void test_growth_is_chosen(void)
{
    env_up_with(true);
    tg_engine_trigger(&eng, now_ns);
    /* A는 400 MiB로 가만히 있고 B는 윈도 안에서 50 MiB에서 200 MiB로 늘어난다 */
    for (int i = 0; i < 5; i++) {
        set_current("B", MIB(50) + MIB(40) * (uint64_t)i);
        ticks(1, FULL60_US);
    }
    TG_CHECK_EQ_U64(eng.st.dry_kills, 1);
    env_down();
    TG_CHECK(strstr(logbuf, "judge kill cand=B") != NULL);
    TG_CHECK(strstr(logbuf, "growing=1") != NULL);
}

static void test_targets_full_is_reported(void)
{
    char name[16];

    env_up_with(true);
    tg_scanner_stop(&sc); /* 표를 다시 만들 수 있도록 멈춘 뒤 후보를 더한다 */
    for (int i = 0; i < 34; i++) {
        snprintf(name, sizeof(name), "X%02d", i);
        mk_cg(name, MIB(1));
    }
    TG_CHECK(tg_scanner_start(&sc, parent, 60000) == 0);
    tg_engine_trigger(&eng, now_ns);
    ticks(2, 0);
    ticks(1, 0); /* 같은 관찰에서는 한 번만 알린다 */
    env_down();
    TG_CHECK(strstr(logbuf, "targets full max=32 skipped=4") != NULL);
    TG_CHECK(strstr(strstr(logbuf, "targets full") + 1, "targets full") == NULL);
}

static void test_late_tick(void)
{
    env_up_with(true);
    tg_engine_trigger(&eng, now_ns);
    ticks(1, 0);
    /* 예정보다 3틱 늦게 도착한다 */
    now_ns += 3 * TICK_NS;
    tg_engine_tick(&eng, now_ns);
    TG_CHECK_EQ_U64(eng.st.late_ticks, 1);
    TG_CHECK(eng.st.max_delay_ns >= 3 * TICK_NS);
    /* 따라잡으려고 연달아 읽지 않고 지금부터 한 틱 뒤로 잡는다 */
    TG_CHECK_EQ_U64(tg_engine_deadline(&eng), now_ns + TICK_NS);
    env_down();
    TG_CHECK(strstr(logbuf, "loop late") != NULL);
}

static void test_sample_error(void)
{
    env_up_with(true);
    tg_engine_trigger(&eng, now_ns);
    write_file(psi_path, "garbage\n");
    tg_engine_tick(&eng, now_ns);
    now_ns += TICK_NS;
    tg_engine_tick(&eng, now_ns);
    TG_CHECK_EQ_U64(eng.st.sample_errors, 2);
    TG_CHECK(tg_engine_watching(&eng)); /* 일시적 읽기 실패로 관찰을 끊지 않는다 */
    env_down();
    TG_CHECK(strstr(logbuf, "sample read failed") != NULL);
}

/* ---------- 이벤트 루프 ---------- */

static void *send_term(void *arg)
{
    (void)arg;
    usleep(300 * 1000);
    kill(getpid(), SIGTERM);
    return NULL;
}

static void test_loop_signal_stops(void)
{
    struct tg_loop_fds f;
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    env_up_with(true);
    TG_CHECK(efd >= 0);
    f.sig_fd = sig_fd;
    f.trigger_fd = efd;
    f.trigger_events = POLLIN;
    kill(getpid(), SIGTERM); /* 막아 둔 신호는 signalfd로 들어온다 */
    TG_CHECK(tg_loop_run(&eng, &f) == 0);
    TG_CHECK_EQ_U64(eng.st.triggers, 0);
    close(efd);
    env_down();
}

static void test_loop_trigger_then_stop(void)
{
    struct tg_loop_fds f;
    pthread_t th;
    uint64_t one = 1;
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    env_up_with(true);
    TG_CHECK(efd >= 0);
    f.sig_fd = sig_fd;
    f.trigger_fd = efd;
    f.trigger_events = POLLIN;
    TG_CHECK(write(efd, &one, sizeof(one)) == (ssize_t)sizeof(one));
    TG_CHECK(pthread_create(&th, NULL, send_term, NULL) == 0);
    TG_CHECK(tg_loop_run(&eng, &f) == 0);
    pthread_join(th, NULL);
    TG_CHECK_EQ_U64(eng.st.triggers, 1);
    TG_CHECK(eng.st.ticks >= 1); /* 트리거 뒤 첫 틱을 바로 처리하고 이후 틱도 이어진다 */
    close(efd);
    env_down();
}

static void test_trigger_open_failure(void)
{
    /* 범위 밖 값은 커널이 거부한다(또는 권한이 없다). 어느 쪽이든 음수 errno여야 한다 */
    int fd = tg_psi_trigger_open(5000000, 1000000);

    TG_CHECK(fd < 0);
    if (fd >= 0)
        close(fd);
}

int main(void)
{
    /* 어떤 스레드(로그, 스캔)보다 먼저 신호를 막는다. 데몬 시작 순서와 같다 */
    sig_fd = tg_signals_block();
    TG_CHECK(sig_fd >= 0);

    snprintf(root, sizeof(root), "/tmp/tg_engine_XXXXXX");
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(psi_path, sizeof(psi_path), "%s/psi", root);
    snprintf(mem_path, sizeof(mem_path), "%s/meminfo", root);
    snprintf(parent, sizeof(parent), "%s/cg", root);

    TG_RUN(test_sample_collect);
    TG_RUN(test_trigger_schedule);
    TG_RUN(test_calm);
    TG_RUN(test_dry_run_kill);
    TG_RUN(test_real_kill);
    TG_RUN(test_kill_failure_keeps_policy);
    TG_RUN(test_mem_available_guard);
    TG_RUN(test_growth_is_chosen);
    TG_RUN(test_targets_full_is_reported);
    TG_RUN(test_late_tick);
    TG_RUN(test_sample_error);
    TG_RUN(test_loop_signal_stops);
    TG_RUN(test_loop_trigger_then_stop);
    TG_RUN(test_trigger_open_failure);

    {
        char cmd[128];

        snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
        if (system(cmd) != 0)
            fprintf(stderr, "임시 디렉터리 정리 실패: %s\n", root);
    }
    return tg_test_finish();
}
