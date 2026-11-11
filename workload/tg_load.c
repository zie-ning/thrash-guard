/*
 * tg-load - 스래싱 재현용 합성 부하
 *
 * 모드마다 별도 프로세스로 실행한다. 그래야 컨테이너 안(anon, file)과 밖(canary)에
 * 따로 놓을 수 있다.
 *
 *   anon    익명 메모리를 일정 속도로 늘려 목표 크기까지 키우고, 전체를 무작위로 계속 쓴다.
 *   file    파일을 mmap해 전체를 무작위로 계속 읽는다. 곧 다시 필요한 파일 페이지를 만든다.
 *   pin     메모리를 잡아 mlock으로 고정하고 가만히 있는다. 커널이 회수할 수 없는 메모리를 모사한다
 *           (길벗 장애 때 unevictable 약 43 MiB, multipathd 등이 mlock한 것으로 추정).
 *   canary  TCP로 요청에 응답하고 처리량·처리 시간을 기록한다. 메모리 보호(mlock)는 하지 않아
 *           일반 앱처럼 스래싱 피해를 그대로 받는다.
 *
 * 1초마다 표준 출력에 한 줄을 쓴다(key=value). 첫 토큰은 "tg-load", 둘째는 mode=...
 *   touches / reads / reqs : 그 1초 동안 처리한 횟수 (처리량)
 *   max_batch_ms / max_req_ms : 그 1초 안에서 가장 오래 걸린 한 묶음/한 요청 (멈춤의 정도)
 * 부하 자체가 스래싱으로 멈추면 출력도 멈춘다. 출력이 끊긴 구간이 곧 피해 구간이다.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PAGE_SZ 4096UL
#define MIB (1024UL * 1024UL)
#define BATCH 256 /* 시간을 확인하는 단위(접근 횟수) */

enum mode { MODE_NONE, MODE_ANON, MODE_FILE, MODE_CANARY, MODE_PIN };

struct opts {
    enum mode mode;
    unsigned long size_mib;  /* anon: 목표 크기, file: 파일 크기, pin: 고정할 크기 */
    unsigned long rate_mib;  /* anon: 초당 증가량. 0이면 즉시 */
    unsigned long duration;  /* 초. 0이면 신호가 올 때까지 */
    unsigned long port;      /* canary */
    unsigned long work_kib;  /* canary: 요청마다 건드리는 작업 메모리 크기 */
    const char *path;        /* file */
    int prepare_only;        /* file: 파일만 만들고 끝낸다 */
    unsigned long seed;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void usage(void)
{
    fprintf(stderr,
            "사용법: tg-load <anon|file|canary|pin> [옵션]\n"
            "  anon   --size-mib N [--rate-mib N] [--duration S]\n"
            "         목표 크기까지 초당 rate-mib씩 늘리며 무작위로 쓴다(rate 0이면 즉시)\n"
            "  file   --path P --size-mib N [--duration S] [--prepare-only]\n"
            "         파일을 만들고(이미 크기가 맞으면 재사용) 무작위로 읽는다\n"
            "  pin    --size-mib N [--duration S]\n"
            "         메모리 N MiB를 잡아 mlock으로 고정하고 가만히 있는다(root 또는 CAP_IPC_LOCK 필요)\n"
            "  canary --port N [--work-kib N] [--duration S]\n"
            "         연결마다 \"ok <번호>\"를 한 줄 답한다\n"
            "  공통   --seed N  난수 시드(기본 1)\n");
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 작은 난수 발생기. 재현성을 위해 시드만 같으면 같은 순서를 만든다. */
static uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static int parse_ul(const char *s, unsigned long max, unsigned long *out)
{
    char *end = NULL;
    unsigned long v;

    if (s == NULL || *s == '\0' || *s == '-')
        return -1;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || *end != '\0' || v > max)
        return -1;
    *out = v;
    return 0;
}

static int parse_args(int argc, char **argv, struct opts *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->seed = 1;
    o->work_kib = 4096;
    if (argc < 2)
        return -1;
    if (strcmp(argv[1], "anon") == 0)
        o->mode = MODE_ANON;
    else if (strcmp(argv[1], "file") == 0)
        o->mode = MODE_FILE;
    else if (strcmp(argv[1], "canary") == 0)
        o->mode = MODE_CANARY;
    else if (strcmp(argv[1], "pin") == 0)
        o->mode = MODE_PIN;
    else
        return -1;

    for (i = 2; i < argc; i++) {
        const char *k = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        int rc = 0;

        if (strcmp(k, "--prepare-only") == 0) {
            o->prepare_only = 1;
            continue;
        }
        if (v == NULL)
            return -1;
        i++;
        if (strcmp(k, "--size-mib") == 0)
            rc = parse_ul(v, 1UL << 20, &o->size_mib); /* 최대 1 TiB */
        else if (strcmp(k, "--rate-mib") == 0)
            rc = parse_ul(v, 1UL << 20, &o->rate_mib);
        else if (strcmp(k, "--duration") == 0)
            rc = parse_ul(v, 86400UL * 7, &o->duration);
        else if (strcmp(k, "--port") == 0)
            rc = parse_ul(v, 65535, &o->port);
        else if (strcmp(k, "--work-kib") == 0)
            rc = parse_ul(v, 1UL << 20, &o->work_kib);
        else if (strcmp(k, "--seed") == 0)
            rc = parse_ul(v, ~0UL >> 1, &o->seed);
        else if (strcmp(k, "--path") == 0)
            o->path = v;
        else
            return -1;
        if (rc != 0) {
            fprintf(stderr, "잘못된 값: %s %s\n", k, v);
            return -2;
        }
    }

    if (o->seed == 0)
        o->seed = 1; /* xorshift는 0 상태가 고정점이다 */
    if (o->mode == MODE_ANON && o->size_mib == 0) {
        fprintf(stderr, "anon: --size-mib가 필요하다\n");
        return -2;
    }
    if (o->mode == MODE_PIN && o->size_mib == 0) {
        fprintf(stderr, "pin: --size-mib가 필요하다\n");
        return -2;
    }
    if (o->mode == MODE_FILE && (o->path == NULL || o->size_mib == 0)) {
        fprintf(stderr, "file: --path와 --size-mib가 필요하다\n");
        return -2;
    }
    if (o->mode == MODE_CANARY && o->port == 0) {
        fprintf(stderr, "canary: --port가 필요하다\n");
        return -2;
    }
    if (o->mode == MODE_CANARY && o->work_kib * 1024UL < PAGE_SZ) {
        fprintf(stderr, "canary: --work-kib는 4 이상이어야 한다\n");
        return -2;
    }
    return 0;
}

/* 1초 단위 통계. 모드마다 쓰는 필드만 채운다. */
struct stats {
    int64_t t0_ms;
    int64_t last_ms;
    uint64_t ops;          /* 이번 구간 처리 횟수 */
    int64_t max_unit_ms;   /* 이번 구간에서 가장 오래 걸린 한 묶음/요청 */
};

static void stats_init(struct stats *s)
{
    s->t0_ms = now_ms();
    s->last_ms = s->t0_ms;
    s->ops = 0;
    s->max_unit_ms = 0;
}

/* 구간이 1초 지났으면 true를 돌려주고 구간을 초기화한다. 호출자가 줄을 출력한다. */
static int stats_due(struct stats *s, int64_t now, int64_t *elapsed_ms, int64_t *since_start_ms)
{
    if (now - s->last_ms < 1000)
        return 0;
    *elapsed_ms = now - s->last_ms;
    *since_start_ms = now - s->t0_ms;
    s->last_ms = now;
    return 1;
}

static int duration_over(const struct opts *o, const struct stats *s, int64_t now)
{
    return o->duration != 0 && (now - s->t0_ms) >= (int64_t)o->duration * 1000;
}

static int run_anon(const struct opts *o)
{
    size_t total = (size_t)o->size_mib * MIB;
    size_t total_pages = total / PAGE_SZ;
    size_t grown = 0;
    uint64_t rng = o->seed;
    uint64_t touches_total = 0;
    struct stats st;
    uint8_t *mem;

    /* NORESERVE: 예약 검사를 건너뛰고, 실제 접근한 만큼만 메모리를 쓴다. */
    mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    stats_init(&st);

    while (!stop_requested) {
        int64_t batch_start = now_ms();
        int64_t batch_end;
        int64_t elapsed;
        int64_t since;
        size_t want;
        int i;

        /* 증가: 경과 시간에 맞춰 새 페이지를 순서대로 채운다. */
        if (o->rate_mib == 0)
            want = total_pages;
        else
            want = (size_t)(((batch_start - st.t0_ms) * (int64_t)o->rate_mib * (int64_t)MIB / 1000) /
                            (int64_t)PAGE_SZ);
        if (want > total_pages)
            want = total_pages;
        while (grown < want && grown < total_pages) {
            mem[grown * PAGE_SZ] = 1;
            grown++;
            if (!(grown % BATCH))
                break; /* 시간 확인 주기를 지키기 위해 한 묶음씩 처리 */
        }

        /* 접근: 지금까지 늘린 영역 전체를 무작위로 쓴다. */
        if (grown > 0) {
            for (i = 0; i < BATCH; i++) {
                size_t idx = (size_t)(rng_next(&rng) % grown);

                mem[idx * PAGE_SZ + (touches_total & 63)]++;
                touches_total++;
                st.ops++;
            }
        } else {
            usleep(1000);
        }

        batch_end = now_ms();
        if (batch_end - batch_start > st.max_unit_ms)
            st.max_unit_ms = batch_end - batch_start;
        if (stats_due(&st, batch_end, &elapsed, &since)) {
            printf("tg-load mode=anon t_s=%" PRId64 " alloc_mib=%zu touches=%" PRIu64
                   " max_batch_ms=%" PRId64 "\n",
                   since / 1000, grown * PAGE_SZ / MIB, st.ops, st.max_unit_ms);
            fflush(stdout);
            st.ops = 0;
            st.max_unit_ms = 0;
        }
        if (duration_over(o, &st, batch_end))
            break;
    }
    munmap(mem, total);
    return 0;
}

/* 파일을 만들거나 재사용한다. 내용은 0이 아닌 값으로 채워 구멍 파일(디스크를 읽지 않는 파일)을 피한다. */
static int prepare_file(const char *path, size_t size)
{
    struct stat sb;
    static uint8_t chunk[MIB];
    size_t done = 0;
    int fd;

    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        perror("open");
        return -1;
    }
    if (fstat(fd, &sb) == 0 && (size_t)sb.st_size == size) {
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        return fd;
    }
    if (ftruncate(fd, 0) != 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }
    memset(chunk, 0xA5, sizeof(chunk));
    while (done < size) {
        size_t n = size - done < sizeof(chunk) ? size - done : sizeof(chunk);
        ssize_t w = write(fd, chunk, n);

        if (w < 0) {
            perror("write");
            close(fd);
            return -1;
        }
        done += (size_t)w;
    }
    fsync(fd);
    /* 만든 직후의 페이지 캐시를 비워, 측정은 디스크에서 읽는 상태로 시작한다. */
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    return fd;
}

static int run_file(const struct opts *o)
{
    size_t size = (size_t)o->size_mib * MIB;
    size_t pages = size / PAGE_SZ;
    uint64_t rng = o->seed;
    volatile uint8_t sink = 0;
    struct stats st;
    uint8_t *mem;
    int fd;

    fd = prepare_file(o->path, size);
    if (fd < 0)
        return 1;
    if (o->prepare_only) {
        close(fd);
        fprintf(stderr, "tg-load: 파일 준비 완료 (%lu MiB, %s)\n", o->size_mib, o->path);
        return 0;
    }
    mem = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    close(fd);
    stats_init(&st);

    while (!stop_requested) {
        int64_t b0 = now_ms();
        int64_t b1;
        int64_t elapsed;
        int64_t since;
        int i;

        for (i = 0; i < BATCH; i++) {
            sink ^= mem[(size_t)(rng_next(&rng) % pages) * PAGE_SZ];
            st.ops++;
        }
        b1 = now_ms();
        if (b1 - b0 > st.max_unit_ms)
            st.max_unit_ms = b1 - b0;
        if (stats_due(&st, b1, &elapsed, &since)) {
            printf("tg-load mode=file t_s=%" PRId64 " file_mib=%lu reads=%" PRIu64
                   " max_batch_ms=%" PRId64 "\n",
                   since / 1000, o->size_mib, st.ops, st.max_unit_ms);
            fflush(stdout);
            st.ops = 0;
            st.max_unit_ms = 0;
        }
        if (duration_over(o, &st, b1))
            break;
    }
    (void)sink;
    munmap(mem, size);
    return 0;
}

/* 메모리를 잡아 모든 페이지를 채우고 mlock으로 고정한다. 그 뒤로는 1초마다 상태만 출력한다. */
static int run_pin(const struct opts *o)
{
    size_t size = (size_t)o->size_mib * MIB;
    size_t i;
    struct stats st;
    uint8_t *mem;

    mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    for (i = 0; i < size; i += PAGE_SZ)
        mem[i] = 1;
    if (mlock(mem, size) != 0) {
        perror("mlock");
        munmap(mem, size);
        return 1;
    }
    stats_init(&st);

    while (!stop_requested) {
        int64_t now;
        int64_t elapsed;
        int64_t since;

        usleep(100000);
        now = now_ms();
        if (stats_due(&st, now, &elapsed, &since)) {
            printf("tg-load mode=pin t_s=%" PRId64 " pinned_mib=%lu\n", since / 1000, o->size_mib);
            fflush(stdout);
        }
        if (duration_over(o, &st, now))
            break;
    }
    munlock(mem, size);
    munmap(mem, size);
    return 0;
}

static int run_canary(const struct opts *o)
{
    size_t work = (size_t)o->work_kib * 1024UL;
    size_t work_pages = work / PAGE_SZ;
    uint64_t rng = o->seed;
    uint64_t seq = 0;
    struct sockaddr_in sa;
    struct stats st;
    uint8_t *mem;
    int one = 1;
    int ls;

    /* 일반 앱처럼 보호하지 않는다. 작업 메모리는 한 번 채워 두어 회수 대상이 되게 한다. */
    mem = mmap(NULL, work, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(mem, 1, work);

    ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (ls < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)o->port);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(ls, 128) != 0) {
        perror("bind/listen");
        close(ls);
        return 1;
    }
    stats_init(&st);

    while (!stop_requested) {
        struct pollfd pfd = { .fd = ls, .events = POLLIN };
        int64_t now;
        int64_t elapsed;
        int64_t since;
        int rc = poll(&pfd, 1, 200);

        if (rc > 0 && (pfd.revents & POLLIN)) {
            int64_t r0 = now_ms();
            int cs = accept4(ls, NULL, NULL, SOCK_CLOEXEC);

            if (cs >= 0) {
                struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
                char req[64];
                char resp[48];
                int n;
                int j;

                setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                /* 요청 내용은 쓰지 않는다. 클라이언트가 보낸 한 줄을 비우기만 한다. */
                if (read(cs, req, sizeof(req)) >= 0) {
                    for (j = 0; j < 16; j++)
                        mem[(size_t)(rng_next(&rng) % work_pages) * PAGE_SZ]++;
                    n = snprintf(resp, sizeof(resp), "ok %" PRIu64 "\n", seq++);
                    if (n > 0 && write(cs, resp, (size_t)n) < 0)
                        perror("write");
                }
                close(cs);
                st.ops++;
                if (now_ms() - r0 > st.max_unit_ms)
                    st.max_unit_ms = now_ms() - r0;
            }
        }
        now = now_ms();
        if (stats_due(&st, now, &elapsed, &since)) {
            printf("tg-load mode=canary t_s=%" PRId64 " reqs=%" PRIu64 " max_req_ms=%" PRId64 "\n",
                   since / 1000, st.ops, st.max_unit_ms);
            fflush(stdout);
            st.ops = 0;
            st.max_unit_ms = 0;
        }
        if (duration_over(o, &st, now))
            break;
    }
    close(ls);
    munmap(mem, work);
    return 0;
}

int main(int argc, char **argv)
{
    struct opts o;
    struct sigaction sa;
    int rc = parse_args(argc, argv, &o);

    if (rc != 0) {
        if (rc == -1)
            usage();
        return 2;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    switch (o.mode) {
    case MODE_ANON:
        return run_anon(&o);
    case MODE_FILE:
        return run_file(&o);
    case MODE_CANARY:
        return run_canary(&o);
    case MODE_PIN:
        return run_pin(&o);
    default:
        usage();
        return 2;
    }
}
