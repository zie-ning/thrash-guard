/*
 * tg-sampler - 스래싱 중에도 버티는 메모리 지표 수집기
 *
 * 일정 주기로 /proc/pressure/memory, /proc/vmstat, /proc/meminfo를 읽어
 * 한 줄짜리 텍스트로 만들고, UDP로 VM 밖 수신기에 보낸다.
 * 디스크에는 쓰지 않는다. 스래싱 중 디스크는 가장 막히는 자원이기 때문이다.
 *
 * 초기화가 끝난 뒤의 루프(핵심 경로)는 힙 할당과 stdio를 쓰지 않는다.
 *
 * 출력 줄 형식 (공백으로 구분한 key=value, 첫 토큰은 형식 버전):
 *   tg1 kind=sample seq=.. mono_us=.. late_us=.. read_us=.. err=.. psi_some_total=.. ...
 *
 *   late_us : 예정 시각보다 늦게 깨어난 시간. 수집기 자신이 멈췄는지 보여 준다
 *   read_us : 세 파일을 읽고 해석하는 데 걸린 시간
 *   skipped : 늦어져서 건너뛴 주기 수(누적)
 *   err     : 읽기 실패 비트 (1 PSI, 2 vmstat, 4 meminfo)
 */
#include "fmt.h"
#include "procfs.h"
#include "selfprot.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LINE_CAP 1024
#define READ_CAP (32 * 1024)

enum {
    ERR_PSI = 1,
    ERR_VMSTAT = 2,
    ERR_MEMINFO = 4,
};

struct config {
    const char *dest;    /* "IP:PORT". NULL이면 표준 출력 */
    long interval_ms;
    int mlock;
};

/* 초기화 때 준비하고 루프에서는 재사용만 하는 상태 */
static struct {
    int psi_fd;
    int vmstat_fd;
    int meminfo_fd;
    int out_fd;      /* UDP 소켓 또는 표준 출력 */
    int is_udp;
    char read_buf[READ_CAP];
    char line[LINE_CAP];
} st;

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void usage(void)
{
    fprintf(stderr,
            "사용법: tg-sampler [--dest IP:PORT] [--interval-ms N] [--no-mlock]\n"
            "  --dest         수신기 주소. 생략하면 표준 출력으로 쓴다\n"
            "  --interval-ms  수집 주기(밀리초, 50~10000, 기본 250)\n"
            "  --no-mlock     mlockall을 하지 않는다(개발용)\n");
}

static int parse_args(int argc, char **argv, struct config *cfg)
{
    int i;

    cfg->dest = NULL;
    cfg->interval_ms = 250;
    cfg->mlock = 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dest") == 0 && i + 1 < argc) {
            cfg->dest = argv[++i];
        } else if (strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) {
            char *end = NULL;
            cfg->interval_ms = strtol(argv[++i], &end, 10);
            if (end == NULL || *end != '\0' || cfg->interval_ms < 50 || cfg->interval_ms > 10000) {
                fprintf(stderr, "잘못된 --interval-ms: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--no-mlock") == 0) {
            cfg->mlock = 0;
        } else {
            usage();
            return -1;
        }
    }
    return 0;
}

static int open_udp(const char *dest)
{
    char host[64];
    const char *colon = strrchr(dest, ':');
    struct sockaddr_in sa;
    char *end = NULL;
    long port;
    size_t hlen;
    int fd;

    if (colon == NULL || (hlen = (size_t)(colon - dest)) == 0 || hlen >= sizeof(host)) {
        fprintf(stderr, "잘못된 --dest: %s (IP:PORT 형식)\n", dest);
        return -1;
    }
    memcpy(host, dest, hlen);
    host[hlen] = '\0';
    port = strtol(colon + 1, &end, 10);
    if (end == NULL || *end != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr, "잘못된 포트: %s\n", colon + 1);
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "IPv4 주소가 아니다: %s\n", host);
        return -1;
    }

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    /* connect해 두면 루프에서 주소 없이 send만 하면 된다. */
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

static int open_ro(const char *path, int required)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0 && required)
        fprintf(stderr, "%s 열기 실패: %s\n", path, strerror(errno));
    return fd;
}

static int64_t mono_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void ts_from_us(struct timespec *ts, int64_t us)
{
    ts->tv_sec = (time_t)(us / 1000000);
    ts->tv_nsec = (long)(us % 1000000) * 1000;
}

/* 핵심 경로: 할당 금지 */
static void emit(size_t len, uint64_t *send_err)
{
    ssize_t n;

    if (st.is_udp) {
        n = send(st.out_fd, st.line, len, MSG_DONTWAIT);
    } else {
        st.line[len] = '\n';
        n = write(st.out_fd, st.line, len + 1);
    }
    if (n < 0)
        (*send_err)++;
}

/* 핵심 경로: 할당 금지 */
static void sample_once(uint64_t seq, int64_t now_us, int64_t late_us, uint64_t skipped,
                        uint64_t *send_err)
{
    struct tg_psi psi;
    struct tg_vmstat vm;
    struct tg_meminfo mi;
    struct tg_buf b;
    unsigned err = 0;
    size_t len = 0;
    int64_t t0 = mono_us();
    int64_t read_us;

    memset(&psi, 0, sizeof(psi));
    memset(&vm, 0, sizeof(vm));
    memset(&mi, 0, sizeof(mi));

    if (tg_read_fd(st.psi_fd, st.read_buf, sizeof(st.read_buf), &len) != 0 ||
        tg_psi_parse(st.read_buf, len, &psi) != 0)
        err |= ERR_PSI;
    if (st.vmstat_fd < 0 ||
        tg_read_fd(st.vmstat_fd, st.read_buf, sizeof(st.read_buf), &len) != 0 ||
        tg_vmstat_parse(st.read_buf, len, &vm, NULL) != 0)
        err |= ERR_VMSTAT;
    if (st.meminfo_fd < 0 ||
        tg_read_fd(st.meminfo_fd, st.read_buf, sizeof(st.read_buf), &len) != 0 ||
        tg_meminfo_parse(st.read_buf, len, &mi, NULL) != 0)
        err |= ERR_MEMINFO;
    read_us = mono_us() - t0;

    /* 마지막 한 바이트는 표준 출력용 줄바꿈 자리로 남긴다. */
    tg_buf_init(&b, st.line, sizeof(st.line) - 1);
    tg_buf_str(&b, "tg1");
    tg_buf_str(&b, " kind=sample");
    tg_buf_kv_u64(&b, "seq", seq);
    tg_buf_kv_i64(&b, "mono_us", now_us);
    tg_buf_kv_i64(&b, "late_us", late_us);
    tg_buf_kv_i64(&b, "read_us", read_us);
    tg_buf_kv_u64(&b, "skipped", skipped);
    tg_buf_kv_u64(&b, "send_err", *send_err);
    tg_buf_kv_u64(&b, "err", err);

    tg_buf_kv_u64(&b, "psi_some_total", psi.some.total_us);
    tg_buf_kv_u64(&b, "psi_full_total", psi.full.total_us);
    tg_buf_kv_u64(&b, "psi_some_avg10", psi.some.avg10);
    tg_buf_kv_u64(&b, "psi_full_avg10", psi.full.avg10);

    tg_buf_kv_u64(&b, "mem_total_kb", mi.mem_total_kb);
    tg_buf_kv_u64(&b, "mem_avail_kb", mi.mem_available_kb);
    tg_buf_kv_u64(&b, "mem_free_kb", mi.mem_free_kb);
    tg_buf_kv_u64(&b, "anon_kb", mi.active_anon_kb + mi.inactive_anon_kb);
    tg_buf_kv_u64(&b, "file_kb", mi.active_file_kb + mi.inactive_file_kb);
    tg_buf_kv_u64(&b, "swap_total_kb", mi.swap_total_kb);
    tg_buf_kv_u64(&b, "swap_free_kb", mi.swap_free_kb);
    tg_buf_kv_u64(&b, "dirty_kb", mi.dirty_kb);
    tg_buf_kv_u64(&b, "writeback_kb", mi.writeback_kb);

    tg_buf_kv_u64(&b, "pgscan_kswapd", vm.pgscan_kswapd);
    tg_buf_kv_u64(&b, "pgscan_direct", vm.pgscan_direct);
    tg_buf_kv_u64(&b, "pgsteal_kswapd", vm.pgsteal_kswapd);
    tg_buf_kv_u64(&b, "pgsteal_direct", vm.pgsteal_direct);
    tg_buf_kv_u64(&b, "pgscan_anon", vm.pgscan_anon);
    tg_buf_kv_u64(&b, "pgscan_file", vm.pgscan_file);
    tg_buf_kv_u64(&b, "pgsteal_anon", vm.pgsteal_anon);
    tg_buf_kv_u64(&b, "pgsteal_file", vm.pgsteal_file);
    tg_buf_kv_u64(&b, "refault_anon", vm.workingset_refault_anon);
    tg_buf_kv_u64(&b, "refault_file", vm.workingset_refault_file);
    tg_buf_kv_u64(&b, "pswpin", vm.pswpin);
    tg_buf_kv_u64(&b, "pswpout", vm.pswpout);
    tg_buf_kv_u64(&b, "pgmajfault", vm.pgmajfault);
    tg_buf_kv_u64(&b, "allocstall", vm.allocstall_normal + vm.allocstall_movable);
    tg_buf_kv_u64(&b, "oom_kill", vm.oom_kill);

    emit(b.len, send_err);
}

static int init(const struct config *cfg)
{
    struct tg_selfprot_opts opts = {
        .mlock = cfg->mlock != 0,
        .stack_prefault = 64 * 1024,
        .oom_score_adj = -1000,
    };
    struct tg_selfprot_result res;
    struct tg_selfmem mem;
    struct sigaction sa;

    st.psi_fd = open_ro("/proc/pressure/memory", 1);
    if (st.psi_fd < 0)
        return -1;
    st.vmstat_fd = open_ro("/proc/vmstat", 1);
    st.meminfo_fd = open_ro("/proc/meminfo", 1);

    if (cfg->dest != NULL) {
        st.out_fd = open_udp(cfg->dest);
        if (st.out_fd < 0)
            return -1;
        st.is_udp = 1;
    } else {
        st.out_fd = STDOUT_FILENO;
        st.is_udp = 0;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 버퍼와 열린 파일이 모두 준비된 뒤 고정해야 그 메모리까지 고정된다. */
    tg_selfprot_apply(&opts, &res);

    fprintf(stderr, "tg-sampler: 주기 %ld ms, 출력 %s\n", cfg->interval_ms,
            cfg->dest != NULL ? cfg->dest : "표준 출력");
    if (res.mlock_err > 0)
        fprintf(stderr, "tg-sampler: 경고: mlockall 실패: %s\n", strerror(res.mlock_err));
    if (res.oom_adj_err > 0)
        fprintf(stderr, "tg-sampler: 경고: oom_score_adj 설정 실패: %s\n",
                strerror(res.oom_adj_err));
    if (tg_selfmem_read(&mem) == 0)
        fprintf(stderr, "tg-sampler: VmRSS %llu kB, VmLck %llu kB\n",
                (unsigned long long)mem.vm_rss_kb, (unsigned long long)mem.vm_lck_kb);
    return 0;
}

int main(int argc, char **argv)
{
    struct config cfg;
    int64_t interval_us;
    int64_t next_us;
    uint64_t seq = 0;
    uint64_t skipped = 0;
    uint64_t send_err = 0;

    if (parse_args(argc, argv, &cfg) != 0)
        return 2;
    if (init(&cfg) != 0)
        return 1;

    interval_us = (int64_t)cfg.interval_ms * 1000;
    next_us = mono_us();

    /* ---------- 핵심 경로: 여기부터 할당 금지 ---------- */
    while (!stop_requested) {
        struct timespec ts;
        int64_t now_us;
        int64_t late_us;

        ts_from_us(&ts, next_us);
        if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
            continue;

        now_us = mono_us();
        late_us = now_us - next_us;
        sample_once(seq++, now_us, late_us, skipped, &send_err);

        /* 늦게 깨어났다면 밀린 주기를 몰아서 수집하지 않고 건너뛴다. */
        next_us += interval_us;
        if (now_us >= next_us) {
            int64_t missed = (now_us - next_us) / interval_us + 1;
            skipped += (uint64_t)missed;
            next_us += missed * interval_us;
        }
    }

    fprintf(stderr, "tg-sampler: 종료 (샘플 %llu개, 전송 실패 %llu개)\n",
            (unsigned long long)seq, (unsigned long long)send_err);
    return 0;
}
