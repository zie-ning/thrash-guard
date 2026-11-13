/*
 * test_noalloc.c - 초기화 이후 힙 할당이 없는지 확인한다
 *
 * malloc 계열 함수를 이 실행 파일에서 직접 정의해(동적 링크라 libc 내부 호출도 가로챈다) 호출 수를
 * 센다. 초기화(로그, 스캔 스레드, 엔진) 뒤에 세기를 켜고, 틱, 스캔 스레드의 재스캔(보호 판정 포함),
 * 종료 결정, 로그 기록 스레드의 출력을 돌린 뒤 0회인지 본다. 센 것은 모든 스레드의 호출이다.
 * sanitizer는 malloc을 가로채므로 이 테스트는 sanitizer 없이 따로 빌드한다.
 * 커널 안에서의 할당은 볼 수 없다(설계 문서의 알려진 한계).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"
#include "loop.h"
#include "mono.h"
#include "tg_test.h"

/* ---------- malloc 가로채기 ---------- */

extern void *__libc_malloc(size_t n);
extern void *__libc_calloc(size_t n, size_t m);
extern void *__libc_realloc(void *p, size_t n);
extern void *__libc_memalign(size_t a, size_t n);

static atomic_bool armed;
static atomic_ulong alloc_calls;

static void note(void)
{
    if (atomic_load_explicit(&armed, memory_order_relaxed))
        atomic_fetch_add_explicit(&alloc_calls, 1, memory_order_relaxed);
}

void *malloc(size_t n)
{
    note();
    return __libc_malloc(n);
}

void *calloc(size_t n, size_t m)
{
    note();
    return __libc_calloc(n, m);
}

void *realloc(void *p, size_t n)
{
    note();
    return __libc_realloc(p, n);
}

void *memalign(size_t a, size_t n);
void *memalign(size_t a, size_t n)
{
    note();
    return __libc_memalign(a, n);
}

int posix_memalign(void **out, size_t a, size_t n)
{
    void *p;

    note();
    p = __libc_memalign(a, n);
    if (p == NULL)
        return ENOMEM;
    *out = p;
    return 0;
}

void *aligned_alloc(size_t a, size_t n)
{
    note();
    return __libc_memalign(a, n);
}

/* ---------- 시험 환경 (test_engine과 같은 가짜 /proc, cgroup) ---------- */

#define MIB(x) ((uint64_t)(x) << 20)
#define TICK_NS 250000000ull
#define FULL60_US 150000ull

static char root[64];
static char psi_path[128];
static char mem_path[128];
static char parent[128];

static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    TG_CHECK(fd >= 0);
    if (fd >= 0) {
        TG_CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
        close(fd);
    }
}

static void mk_cg(const char *name, uint64_t bytes)
{
    char p[256];
    char v[32];
    static const char *const files[] = {"memory.pressure", "cgroup.kill", "cgroup.procs"};

    snprintf(p, sizeof(p), "%s/%s", parent, name);
    TG_CHECK(mkdir(p, 0700) == 0);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s/%s", parent, name, files[i]);
        write_file(p, "");
    }
    snprintf(p, sizeof(p), "%s/%s/memory.current", parent, name);
    snprintf(v, sizeof(v), "%llu\n", (unsigned long long)bytes);
    write_file(p, v);
}

/* 출력은 버린다. 기록 스레드가 sink를 부르는 경로까지 센다 */
static unsigned long sink_bytes;

static int count_sink(void *ctx, const char *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    sink_bytes += len;
    return 0;
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

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

    nanosleep(&ts, NULL);
}

static struct tg_config cfg;
static struct tg_log lg;
static struct tg_scanner sc;
static struct tg_engine eng;

static void test_counter_works(void)
{
    void *(*volatile alloc_fn)(size_t) = malloc; /* 컴파일러가 malloc/free 쌍을 지우지 못하게 한다 */
    void *p;

    atomic_store(&alloc_calls, 0);
    atomic_store(&armed, true);
    p = alloc_fn(16);
    atomic_store(&armed, false);
    free(p);
    TG_CHECK(atomic_load(&alloc_calls) == 1); /* 가로채기가 동작해야 0회라는 결과에 뜻이 있다 */
}

static void test_no_alloc_after_init(void)
{
    struct tg_log_opts o;
    char cmd[256];
    uint64_t now;
    uint64_t full = 0;

    snprintf(cmd, sizeof(cmd), "mkdir %s/cg", root);
    TG_CHECK(system(cmd) == 0);
    mk_cg("A", MIB(400));
    mk_cg("B", MIB(50));
    write_file(psi_path, "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
                         "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
    write_file(mem_path, "MemTotal:        1048576 kB\nMemFree:          100000 kB\n"
                         "MemAvailable:      51200 kB\n");

    tg_config_defaults(&cfg);
    snprintf(cfg.target_parent, sizeof(cfg.target_parent), "%s", parent);
    cfg.sustain_sec = 1;
    cfg.sample_interval_ms = 250;
    cfg.dry_run = false;

    /* ---- 초기화: 여기서는 할당해도 된다 ---- */
    memset(&o, 0, sizeof(o));
    o.nslots = 64;
    o.sink = count_sink;
    TG_CHECK(tg_log_open(&lg, &o) == 0);
    TG_CHECK(tg_scanner_start(&sc, parent, NULL, 50) == 0); /* 50 ms마다 재스캔 */
    TG_CHECK(tg_engine_init(&eng, &cfg, &lg, &sc, psi_path, mem_path) == 0);
    now = tg_now_ns();
    sleep_ms(100); /* 스레드가 자리를 잡게 한다 */

    /* ---- 초기화 이후: 세기를 켠다 ---- */
    atomic_store(&alloc_calls, 0);
    atomic_store(&armed, true);

    tg_engine_trigger(&eng, now);
    for (int i = 0; i < 6; i++) { /* 압력 지속 -> 종료 결정 -> cgroup.kill 쓰기 */
        full += FULL60_US;
        set_full(full);
        tg_engine_tick(&eng, now);
        now += TICK_NS;
    }
    tg_engine_log_summary(&eng);
    sleep_ms(400); /* 스캔 스레드 재스캔(보호 판정 포함)과 기록 스레드 출력이 돌 시간 */

    atomic_store(&armed, false);
    /* ---- 세기 끝 ---- */

    TG_CHECK_EQ_U64(eng.st.kills, 1); /* 종료 경로까지 실제로 지났는지 */
    TG_CHECK(sink_bytes > 0);         /* 기록 스레드가 출력까지 했는지 */
    TG_CHECK_EQ_U64(atomic_load(&alloc_calls), 0);

    tg_engine_close(&eng);
    tg_scanner_stop(&sc);
    tg_log_stop(&lg);
}

int main(void)
{
    /* 어떤 스레드보다 먼저 신호를 막는다 (데몬 시작 순서와 같다) */
    TG_CHECK(tg_signals_block() >= 0);
    snprintf(root, sizeof(root), "/tmp/tg_noalloc_XXXXXX");
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(psi_path, sizeof(psi_path), "%s/psi", root);
    snprintf(mem_path, sizeof(mem_path), "%s/meminfo", root);
    snprintf(parent, sizeof(parent), "%s/cg", root);

    TG_RUN(test_counter_works);
    TG_RUN(test_no_alloc_after_init);
    return tg_test_finish();
}
