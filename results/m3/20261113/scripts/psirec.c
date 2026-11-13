/*
 * psirec - PSI(memory, io, cpu)와 MemAvailable을 1초마다 기록한다
 *
 * 스래싱 중에도 기록이 끊기지 않도록 mlockall로 고정하고, 출력은 tmpfs(/dev/shm) 파일에 write(2)로 이어 쓴다.
 * 파일 하나당 한 줄: 에포크 밀리초, memory some/full, io some/full, cpu some/full (누적 us), MemAvailable(kB).
 * stdio와 힙 할당을 쓰지 않는다.
 *
 * 사용법: psirec <출력 경로> [초]    (초를 생략하면 신호가 올 때까지)
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
    int out;
    unsigned long secs = 0;
    struct timespec next;
    char buf[1024];
    char line[256];

    if (argc < 2)
        return 2;
    if (argc > 2)
        secs = strtoul(argv[2], NULL, 10);
    for (int i = 0; i < 3; i++) {
        fd[i] = open(files[i], O_RDONLY | O_CLOEXEC);
        if (fd[i] < 0)
            return 3;
    }
    mi = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    out = open(argv[1], O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (mi < 0 || out < 0)
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
        line[len++] = '\n';
        if (write(out, line, len) < 0)
            return 4;
        next.tv_sec += 1;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return 0;
}
