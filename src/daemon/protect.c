/*
 * protect.c - 보호된 cgroup 판정 구현
 *
 * 스캔 스레드에서 도는 코드다. 힙 할당과 stdio를 쓰지 않는다.
 */
#include "protect.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* linux_dirent64의 배치: d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name[] */
#define DIRENT_RECLEN_OFF 16
#define DIRENT_TYPE_OFF 18
#define DIRENT_NAME_OFF 19

#define OOM_ADJ_PROTECTED (-1000)
#define PID_MAX_SANE 4294967295ull

/* "<pid>/oom_score_adj"를 만든다. 반환값은 길이. */
static size_t proc_path(char *out, size_t cap, uint64_t pid)
{
    static const char tail[] = "/oom_score_adj";
    char digits[24];
    size_t nd = 0;
    size_t n = 0;

    do {
        digits[nd++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0 && nd < sizeof(digits));
    /* 반복문이 이미 nd를 sizeof(digits) 이하로 막는다. 정적 분석기가 배열 경계를 알도록 한 번 더 자른다. */
    if (nd > sizeof(digits))
        nd = sizeof(digits);
    if (nd + sizeof(tail) > cap)
        return 0;
    for (size_t i = nd; i > 0; i--)
        out[n++] = digits[i - 1];
    memcpy(out + n, tail, sizeof(tail));
    return n + sizeof(tail) - 1;
}

/* 부호 있는 10진수 한 개를 읽는다. 형식이 맞으면 true. */
static bool parse_adj(const char *buf, size_t len, long *out)
{
    size_t i = 0;
    bool neg = false;
    long v = 0;
    bool any = false;

    if (i < len && buf[i] == '-') {
        neg = true;
        i++;
    }
    for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++) {
        v = v * 10 + (buf[i] - '0');
        any = true;
        if (v > 100000)
            return false;
    }
    if (!any)
        return false;
    for (; i < len; i++) {
        if (buf[i] != '\n' && buf[i] != ' ' && buf[i] != '\0')
            return false;
    }
    *out = neg ? -v : v;
    return true;
}

/* 프로세스 하나의 oom_score_adj를 본다. 그 사이 사라졌으면 보호 대상이 아닌 것으로 본다. */
static int check_pid(int proc_fd, uint64_t pid)
{
    char path[48];
    char buf[24];
    ssize_t n;
    long adj;
    int fd;

    if (proc_path(path, sizeof(path), pid) == 0)
        return TG_PROTECT_UNKNOWN;
    do {
        fd = openat(proc_fd, path, O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return (errno == ENOENT || errno == ESRCH) ? TG_PROTECT_NO : TG_PROTECT_UNKNOWN;
    do {
        n = read(fd, buf, sizeof(buf) - 1);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n < 0)
        return (errno == ESRCH) ? TG_PROTECT_NO : TG_PROTECT_UNKNOWN;
    if (!parse_adj(buf, (size_t)n, &adj))
        return TG_PROTECT_UNKNOWN;
    return adj == OOM_ADJ_PROTECTED ? TG_PROTECT_YES : TG_PROTECT_NO;
}

/*
 * dfd 아래 cgroup.procs를 조각으로 읽으며 프로세스마다 확인한다. 목록을 모으지 않으므로
 * 프로세스 수에 상한이 없다.
 */
static int check_procs(int proc_fd, int dfd)
{
    char buf[1024];
    uint64_t pid = 0;
    bool in_num = false;
    int fd;

    do {
        fd = openat(dfd, "cgroup.procs", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return TG_PROTECT_UNKNOWN;

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));

        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return TG_PROTECT_UNKNOWN;
        }
        if (n == 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];

            if (c >= '0' && c <= '9') {
                pid = pid * 10 + (uint64_t)(c - '0');
                in_num = true;
                if (pid > PID_MAX_SANE) {
                    close(fd);
                    return TG_PROTECT_UNKNOWN;
                }
            } else if (c == '\n') {
                if (in_num) {
                    int r = check_pid(proc_fd, pid);

                    if (r != TG_PROTECT_NO) {
                        close(fd);
                        return r;
                    }
                }
                pid = 0;
                in_num = false;
            } else {
                close(fd);
                return TG_PROTECT_UNKNOWN; /* 예상하지 못한 형식 */
            }
        }
    }
    close(fd);
    return in_num ? check_pid(proc_fd, pid) : TG_PROTECT_NO;
}

static int check_dir(int proc_fd, int dfd, int depth)
{
    char buf[2048];
    int r = check_procs(proc_fd, dfd);

    if (r != TG_PROTECT_NO)
        return r;

    for (;;) {
        long n = syscall(SYS_getdents64, dfd, buf, sizeof(buf));
        long off = 0;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return TG_PROTECT_UNKNOWN;
        }
        if (n == 0)
            return TG_PROTECT_NO;

        while (off < n) {
            uint16_t reclen;
            unsigned char type;
            const char *name;
            int sub;

            if (n - off < DIRENT_NAME_OFF + 1)
                return TG_PROTECT_UNKNOWN;
            memcpy(&reclen, buf + off + DIRENT_RECLEN_OFF, sizeof(reclen));
            if (reclen < DIRENT_NAME_OFF + 1 || reclen > n - off)
                return TG_PROTECT_UNKNOWN;
            type = (unsigned char)buf[off + DIRENT_TYPE_OFF];
            name = buf + off + DIRENT_NAME_OFF;
            off += reclen;

            if (type != DT_DIR && type != DT_UNKNOWN)
                continue;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
                continue;
            if (depth >= TG_PROTECT_MAX_DEPTH)
                return TG_PROTECT_UNKNOWN; /* 상한 아래에 하위 cgroup이 더 있다 */

            do {
                sub = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            } while (sub < 0 && errno == EINTR);
            if (sub < 0) {
                if (errno == ENOTDIR || errno == ENOENT)
                    continue; /* 폴더가 아니거나 그 사이 사라졌다 */
                return TG_PROTECT_UNKNOWN;
            }
            r = check_dir(proc_fd, sub, depth + 1);
            close(sub);
            if (r != TG_PROTECT_NO)
                return r;
        }
    }
}

int tg_protect_check(int proc_fd, int parent_fd, const char *name)
{
    int dfd;
    int r;

    if (proc_fd < 0 || parent_fd < 0 || name == NULL)
        return TG_PROTECT_UNKNOWN;
    do {
        dfd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while (dfd < 0 && errno == EINTR);
    if (dfd < 0)
        return TG_PROTECT_UNKNOWN;
    r = check_dir(proc_fd, dfd, 0);
    close(dfd);
    return r;
}

int tg_targets_mark(struct tg_targets *t, int proc_fd, const char *parent, uint64_t now_ns)
{
    int dfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (dfd < 0)
        return -errno;
    for (uint32_t i = 0; i < t->n; i++)
        t->t[i].protect = (uint8_t)tg_protect_check(proc_fd, dfd, t->t[i].name);
    close(dfd);
    t->scanned_ns = now_ns;
    return 0;
}
