/*
 * envcheck.c - 시작 시 실행 환경 점검
 */
#include "envcheck.h"

#include <fcntl.h>
#include <string.h>
#include <sys/statfs.h>
#include <unistd.h>

#ifndef CGROUP2_SUPER_MAGIC
#define CGROUP2_SUPER_MAGIC 0x63677270
#endif

#define PATH_CAP 512

static void add(struct tg_env_report *r, const char *msg)
{
    if (r->n < TG_ENV_MAX_ISSUES)
        r->issue[r->n++] = msg;
}

/* "a/b/c"를 buf에 이어 붙인다. 길면 false. */
static bool join3(char *buf, size_t cap, const char *a, const char *b, const char *c)
{
    size_t la = strlen(a);
    size_t lb = b != NULL ? strlen(b) : 0;
    size_t lc = strlen(c);

    if (la + 1 + lb + 1 + lc + 1 > cap)
        return false;
    memcpy(buf, a, la);
    buf[la] = '/';
    size_t off = la + 1;
    if (b != NULL) {
        memcpy(buf + off, b, lb);
        off += lb;
        buf[off++] = '/';
    }
    memcpy(buf + off, c, lc);
    buf[off + lc] = '\0';
    return true;
}

/* 파일 첫 4 KiB에서 공백으로 구분한 단어 word가 있는지 본다. 읽지 못하면 false. */
static bool file_has_word(const char *path, const char *word)
{
    char buf[4096];
    size_t wl = strlen(word);
    ssize_t n;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return false;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    for (const char *p = buf; *p != '\0';) {
        while (*p == ' ' || *p == '\n')
            p++;
        const char *s = p;
        while (*p != '\0' && *p != ' ' && *p != '\n')
            p++;
        if ((size_t)(p - s) == wl && memcmp(s, word, wl) == 0)
            return true;
    }
    return false;
}

void tg_env_check_files(const char *parent, const char *sample_child, struct tg_env_report *r)
{
    char path[PATH_CAP];

    if (!join3(path, sizeof(path), parent, NULL, "cgroup.subtree_control") ||
        !file_has_word(path, "memory"))
        add(r, "memory controller is not enabled for children of target_parent "
               "(cgroup.subtree_control has no memory)");

    if (sample_child != NULL) {
        if (!join3(path, sizeof(path), parent, sample_child, "cgroup.kill") ||
            access(path, W_OK) != 0)
            add(r, "cgroup.kill is missing or not writable (needs Linux 5.14 or later), "
                   "termination will fail");
    }
}

void tg_env_check(const char *parent, const char *sample_child, struct tg_env_report *r)
{
    struct statfs sf;

    r->n = 0;
    if (statfs(parent, &sf) != 0) {
        add(r, "target_parent cannot be read");
        return;
    }
    if ((unsigned long)sf.f_type != (unsigned long)CGROUP2_SUPER_MAGIC) {
        add(r, "target_parent is not on a cgroup v2 filesystem");
        return;
    }
    tg_env_check_files(parent, sample_child, r);
}
