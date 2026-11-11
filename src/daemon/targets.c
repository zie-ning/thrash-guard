/*
 * targets.c - 종료 후보 cgroup 표 구현
 *
 * 스캔 스레드에서 도는 코드다. 힙 할당과 stdio를 쓰지 않는다.
 */
#include "targets.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* linux_dirent64의 배치: d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name[] */
#define DIRENT_RECLEN_OFF 16
#define DIRENT_NAME_OFF 19

void tg_targets_init(struct tg_targets *t)
{
    memset(t, 0, sizeof(*t));
}

const struct tg_target *tg_targets_find(const struct tg_targets *t, const char *name)
{
    if (t == NULL)
        return NULL;
    for (uint32_t i = 0; i < t->n; i++) {
        if (strcmp(t->t[i].name, name) == 0)
            return &t->t[i];
    }
    return NULL;
}

/* keep에 같은 이름, 같은 fd로 살아 있는 항목인가 */
static bool live_in(const struct tg_targets *keep, const struct tg_target *e)
{
    if (keep == NULL)
        return false;
    for (uint32_t i = 0; i < keep->n; i++) {
        if (keep->t[i].pressure_fd == e->pressure_fd && keep->t[i].stat_fd == e->stat_fd &&
            strcmp(keep->t[i].name, e->name) == 0)
            return true;
    }
    return false;
}

void tg_targets_release(struct tg_targets *t, const struct tg_targets *keep)
{
    for (uint32_t i = 0; i < t->n; i++) {
        struct tg_target *e = &t->t[i];

        if (live_in(keep, e))
            continue;
        if (e->pressure_fd >= 0)
            close(e->pressure_fd);
        if (e->stat_fd >= 0)
            close(e->stat_fd);
        e->pressure_fd = -1;
        e->stat_fd = -1;
    }
    t->n = 0;
    t->skipped = 0;
}

/* dfd 아래 "name/file"을 연다. */
static int open_in_dir(int dfd, const char *name, size_t nlen, const char *file)
{
    char path[TG_TARGET_NAME_CAP + 24];
    size_t flen = strlen(file);

    memcpy(path, name, nlen);
    path[nlen] = '/';
    memcpy(path + nlen + 1, file, flen + 1);
    return openat(dfd, path, O_RDONLY | O_CLOEXEC);
}

static void add_entry(struct tg_targets *next, const struct tg_targets *cur, int dfd,
                      const char *name, size_t nlen, uint64_t now_ns, bool initial)
{
    const struct tg_target *old;
    struct tg_target *e;
    int pfd;
    int sfd;

    if (nlen == 0 || nlen >= TG_TARGET_NAME_CAP)
        return;
    if (name[0] == '.' && (nlen == 1 || (nlen == 2 && name[1] == '.')))
        return;

    old = tg_targets_find(cur, name);
    if (old != NULL) {
        if (next->n >= TG_MAX_TARGETS) {
            next->skipped++;
            return;
        }
        next->t[next->n++] = *old;
        return;
    }

    pfd = open_in_dir(dfd, name, nlen, "memory.pressure");
    if (pfd < 0)
        return; /* 디렉터리가 아니거나 memory 컨트롤러가 없다 */
    if (next->n >= TG_MAX_TARGETS) {
        close(pfd);
        next->skipped++;
        return;
    }
    sfd = open_in_dir(dfd, name, nlen, "memory.stat");
    if (sfd < 0) {
        close(pfd);
        return;
    }

    e = &next->t[next->n++];
    memcpy(e->name, name, nlen);
    e->name[nlen] = '\0';
    e->pressure_fd = pfd;
    e->stat_fd = sfd;
    e->first_seen_ns = initial ? 0 : now_ns;
}

int tg_targets_rebuild(struct tg_targets *next, const struct tg_targets *cur, const char *parent,
                       uint64_t now_ns, bool initial)
{
    char buf[4096];
    int dfd;
    int rc = 0;
    bool done = false;

    tg_targets_release(next, cur);

    dfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0)
        return -errno;

    while (!done) {
        long n = syscall(SYS_getdents64, dfd, buf, sizeof(buf));
        long off = 0;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = -errno;
            break;
        }
        if (n == 0)
            break;

        while (off < n) {
            uint16_t reclen;

            if (n - off < DIRENT_NAME_OFF + 1) {
                rc = -EIO;
                done = true;
                break;
            }
            memcpy(&reclen, buf + off + DIRENT_RECLEN_OFF, sizeof(reclen));
            if (reclen < DIRENT_NAME_OFF + 1 || reclen > n - off) {
                rc = -EIO;
                done = true;
                break;
            }
            add_entry(next, cur, dfd, buf + off + DIRENT_NAME_OFF,
                      strnlen(buf + off + DIRENT_NAME_OFF, (size_t)reclen - DIRENT_NAME_OFF),
                      now_ns, initial);
            off += reclen;
        }
    }
    close(dfd);

    if (rc != 0)
        tg_targets_release(next, cur);
    return rc;
}
