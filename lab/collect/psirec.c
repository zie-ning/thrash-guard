/*
 * psirec - PSI(memory, io, cpu)와 MemAvailable을 1초마다 기록한다
 *
 * 스래싱 중에도 기록이 끊기지 않도록 mlockall로 고정하고, 출력은 tmpfs(/dev/shm) 파일에 write(2)로 이어 쓴다.
 * 한 줄: 에포크 밀리초, memory some/full, io some/full, cpu some/full (누적 us), MemAvailable(kB),
 * vda 읽기 완료 횟수, vda 읽은 섹터 수(512바이트). 마지막 두 값은 처리량 상한이 실제로 걸리는지 보려는 것이다.
 * stdio와 힙 할당을 쓰지 않는다.
 *
 * PID를 셋째 인자로 주면 그 프로세스의 minflt, majflt(누적), VmRSS, VmLck(kB) 네 열을 끝에 더한다(14열).
 * 데몬이 스래싱 중 쫓겨나 디스크에서 다시 읽히는지 보려는 것이다.
 *
 * 사용법: psirec <출력 경로> [초 [PID]]    (초가 0이거나 생략되면 신호가 올 때까지)
 */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_sig(int s)
{
    (void)s;
    stop = 1;
}

/* buf 안에서 key로 시작하는 줄의 "total=" 값을 읽는다. 없으면 0. */
static uint64_t line_total(const char *buf, size_t n, const char *key)
{
    size_t kl = strlen(key);

    for (size_t i = 0; i + kl < n; i++) {
        if ((i == 0 || buf[i - 1] == '\n') && memcmp(buf + i, key, kl) == 0) {
            for (size_t j = i; j + 6 < n && buf[j] != '\n'; j++) {
                if (memcmp(buf + j, "total=", 6) == 0) {
                    uint64_t v = 0;
                    for (size_t k = j + 6; k < n && buf[k] >= '0' && buf[k] <= '9'; k++)
                        v = v * 10 + (uint64_t)(buf[k] - '0');
                    return v;
                }
            }
        }
    }
    return 0;
}

static uint64_t meminfo_avail(const char *buf, size_t n)
{
    static const char key[] = "MemAvailable:";

    for (size_t i = 0; i + sizeof(key) < n; i++) {
        if ((i == 0 || buf[i - 1] == '\n') && memcmp(buf + i, key, sizeof(key) - 1) == 0) {
            uint64_t v = 0;
            size_t k = i + sizeof(key) - 1;
            while (k < n && buf[k] == ' ')
                k++;
            for (; k < n && buf[k] >= '0' && buf[k] <= '9'; k++)
                v = v * 10 + (uint64_t)(buf[k] - '0');
            return v;
        }
    }
    return 0;
}

/* /proc/diskstats에서 장치 이름이 dev인 줄의 읽기 완료 횟수(4번째)와 읽은 섹터(6번째)를 읽는다. 없으면 0. */
static void disk_reads(const char *buf, size_t n, const char *dev, uint64_t *reads, uint64_t *sectors)
{
    size_t dl = strlen(dev);

    *reads = 0;
    *sectors = 0;
    for (size_t i = 0; i < n;) {
        size_t e = i;
        while (e < n && buf[e] != '\n')
            e++;
        /* 필드: major minor name reads_completed reads_merged sectors_read ... */
        size_t pos = i;
        int field = 0;
        uint64_t vals[3] = {0, 0, 0};
        int match = 0;
        while (pos < e && field < 6) {
            while (pos < e && buf[pos] == ' ')
                pos++;
            size_t st = pos;
            while (pos < e && buf[pos] != ' ')
                pos++;
            if (field == 2)
                match = (pos - st == dl && memcmp(buf + st, dev, dl) == 0);
            else if (field == 3 || field == 5) {
                uint64_t v = 0;
                for (size_t k = st; k < pos && buf[k] >= '0' && buf[k] <= '9'; k++)
                    v = v * 10 + (uint64_t)(buf[k] - '0');
                vals[field == 3 ? 0 : 1] = v;
            }
            field++;
        }
        if (match && field >= 6) {
            *reads = vals[0];
            *sectors = vals[1];
            return;
        }
        i = e + 1;
    }
}

/* /proc/<pid>/stat에서 minflt(10번째 필드)와 majflt(12번째)를 읽는다. comm에 공백이 있을 수 있어 마지막 ')' 뒤부터 센다. */
static void pid_faults(const char *buf, size_t n, uint64_t *minflt, uint64_t *majflt)
{
    size_t p = n;
    int field = 2; /* ')'가 2번째 필드의 끝 */

    *minflt = 0;
    *majflt = 0;
    while (p > 0 && buf[p - 1] != ')')
        p--;
    if (p == 0)
        return;
    while (p < n && field < 12) {
        while (p < n && buf[p] == ' ')
            p++;
        field++;
        if (field == 10 || field == 12) {
            uint64_t v = 0;
            for (size_t k = p; k < n && buf[k] >= '0' && buf[k] <= '9'; k++)
                v = v * 10 + (uint64_t)(buf[k] - '0');
            if (field == 10)
                *minflt = v;
            else
                *majflt = v;
        }
        while (p < n && buf[p] != ' ')
            p++;
    }
}

/* /proc/<pid>/status에서 "key:" 줄의 kB 값을 읽는다. 없으면 0. */
static uint64_t status_kb(const char *buf, size_t n, const char *key)
{
    size_t kl = strlen(key);

    for (size_t i = 0; i + kl < n; i++) {
        if ((i == 0 || buf[i - 1] == '\n') && memcmp(buf + i, key, kl) == 0) {
            uint64_t v = 0;
            size_t k = i + kl;
            while (k < n && (buf[k] == ' ' || buf[k] == '\t'))
                k++;
            for (; k < n && buf[k] >= '0' && buf[k] <= '9'; k++)
                v = v * 10 + (uint64_t)(buf[k] - '0');
            return v;
        }
    }
    return 0;
}

static size_t put_u64(char *out, uint64_t v)
{
    char t[24];
    size_t n = 0;
    size_t len = 0;

    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0)
        out[len++] = t[--n];
    return len;
}

int main(int argc, char **argv)
{
    static const char *const files[3] = {"/proc/pressure/memory", "/proc/pressure/io", "/proc/pressure/cpu"};
    int fd[3];
    int mi;
    int ds;
    int out;
    unsigned long secs = 0;
    struct timespec next;
    char buf[1024];
    static char dbuf[16384];
    char line[400];

    int pst = -1; /* 데몬 /proc/<pid>/stat */
    int pss = -1; /* 데몬 /proc/<pid>/status */
    char pbuf[1024];

    if (argc < 2)
        return 2;
    if (argc > 2)
        secs = strtoul(argv[2], NULL, 10);
    if (argc > 3) {
        /* 대상 PID의 page fault와 VmRSS/VmLck 열을 더한다. 프로세스가 사라지면 0을 적는다. */
        char path[64] = "/proc/";
        size_t pl = strlen(path);

        if (strlen(argv[3]) > 10 || argv[3][0] < '0' || argv[3][0] > '9')
            return 2;
        memcpy(path + pl, argv[3], strlen(argv[3]));
        pl += strlen(argv[3]);
        memcpy(path + pl, "/stat", 6);
        pst = open(path, O_RDONLY | O_CLOEXEC);
        memcpy(path + pl, "/status", 8);
        pss = open(path, O_RDONLY | O_CLOEXEC);
        if (pst < 0 || pss < 0)
            return 3;
    }
    for (int i = 0; i < 3; i++) {
        fd[i] = open(files[i], O_RDONLY | O_CLOEXEC);
        if (fd[i] < 0)
            return 3;
    }
    mi = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    ds = open("/proc/diskstats", O_RDONLY | O_CLOEXEC);
    out = open(argv[1], O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (mi < 0 || out < 0 || ds < 0)
        return 3;
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    mlockall(MCL_CURRENT | MCL_FUTURE);

    clock_gettime(CLOCK_MONOTONIC, &next);
    for (unsigned long i = 0; !stop && (secs == 0 || i < secs); i++) {
        struct timespec rt;
        size_t len = 0;
        ssize_t n;

        clock_gettime(CLOCK_REALTIME, &rt);
        len += put_u64(line + len, (uint64_t)rt.tv_sec * 1000 + (uint64_t)rt.tv_nsec / 1000000);
        for (int k = 0; k < 3; k++) {
            n = pread(fd[k], buf, sizeof(buf), 0);
            for (int f = 0; f < 2; f++) {
                line[len++] = ' ';
                len += put_u64(line + len, n > 0 ? line_total(buf, (size_t)n, f == 0 ? "some" : "full") : 0);
            }
        }
        n = pread(mi, buf, sizeof(buf), 0);
        line[len++] = ' ';
        len += put_u64(line + len, n > 0 ? meminfo_avail(buf, (size_t)n) : 0);
        {
            uint64_t rd = 0;
            uint64_t sec = 0;

            n = pread(ds, dbuf, sizeof(dbuf), 0);
            if (n > 0)
                disk_reads(dbuf, (size_t)n, "vda", &rd, &sec);
            line[len++] = ' ';
            len += put_u64(line + len, rd);
            line[len++] = ' ';
            len += put_u64(line + len, sec);
        }
        if (pst >= 0) {
            uint64_t v[4] = {0, 0, 0, 0};

            n = pread(pst, pbuf, sizeof(pbuf), 0);
            if (n > 0)
                pid_faults(pbuf, (size_t)n, &v[0], &v[1]);
            n = pread(pss, pbuf, sizeof(pbuf), 0);
            if (n > 0) {
                v[2] = status_kb(pbuf, (size_t)n, "VmRSS:");
                v[3] = status_kb(pbuf, (size_t)n, "VmLck:");
            }
            for (int k = 0; k < 4; k++) {
                line[len++] = ' ';
                len += put_u64(line + len, v[k]);
            }
        }
        line[len++] = '\n';
        if (write(out, line, len) < 0)
            return 4;
        next.tv_sec += 1;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return 0;
}
